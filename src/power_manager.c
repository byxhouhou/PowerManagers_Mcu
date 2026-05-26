#include "power_manager.h"
#include "power_manager_port.h"

#include <string.h>
#include "semphr.h"
#include "task.h"
#include "timers.h"

#define POWER_MANAGER_QUEUE_LENGTH 8U
#define POWER_MANAGER_MAX_PERIPHERALS 16U

typedef struct
{
    PowerIoId_t io;
    PowerIoLevel_t restore_level;
} PulseRestore_t;

static PowerManagerConfig_t s_config;
static PowerManagerSnapshot_t s_snapshot;
static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static PowerStateChangedCallback_t s_state_callback;
static PowerStateSyncCallback_t s_state_sync_callback;
static TickType_t s_state_enter_tick;
static uint8_t s_state_sync_cycle_count;
static PowerPeripheralState_t s_peripheral_states[POWER_MANAGER_MAX_PERIPHERALS];

/* 每个电源状态对应一组输出电平，状态切换时统一应用，方便审查整车电源时序。 */
static const PowerIoLevel_t s_state_io_table[4][POWER_IO_COUNT] =
{
    [POWER_STATE_SLEEP] =
    {
        [POWER_IO_MAIN_RELAY] = POWER_IO_LEVEL_LOW,
        [POWER_IO_SENSOR_5V_EN] = POWER_IO_LEVEL_LOW,
        [POWER_IO_CAN_STB] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_MCU_HOLD] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_STATUS_LED] = POWER_IO_LEVEL_LOW,
    },
    [POWER_STATE_WAKEUP] =
    {
        [POWER_IO_MAIN_RELAY] = POWER_IO_LEVEL_LOW,
        [POWER_IO_SENSOR_5V_EN] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_CAN_STB] = POWER_IO_LEVEL_LOW,
        [POWER_IO_MCU_HOLD] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_STATUS_LED] = POWER_IO_LEVEL_HIGH,
    },
    [POWER_STATE_WORK] =
    {
        [POWER_IO_MAIN_RELAY] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_SENSOR_5V_EN] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_CAN_STB] = POWER_IO_LEVEL_LOW,
        [POWER_IO_MCU_HOLD] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_STATUS_LED] = POWER_IO_LEVEL_HIGH,
    },
    [POWER_STATE_SHUTDOWN_PREPARE] =
    {
        [POWER_IO_MAIN_RELAY] = POWER_IO_LEVEL_LOW,
        [POWER_IO_SENSOR_5V_EN] = POWER_IO_LEVEL_LOW,
        [POWER_IO_CAN_STB] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_MCU_HOLD] = POWER_IO_LEVEL_HIGH,
        [POWER_IO_STATUS_LED] = POWER_IO_LEVEL_LOW,
    },
};

static const PowerManagerConfig_t s_default_config =
{
    .wakeup_min_ticks = pdMS_TO_TICKS(500),
    .shutdown_prepare_ticks = pdMS_TO_TICKS(2000),
    .state_machine_period_ticks = pdMS_TO_TICKS(50),
    .state_sync_period_cycles = 10,
    .shutdown_required_mask = 0U,
    .peripheral_pm_enabled = false,
    .peripherals = NULL,
    .peripheral_count = 0,
};

static void sync_power_state(PowerStateSyncReason_t reason,
                             const PowerManagerSnapshot_t *snapshot)
{
    if (s_state_sync_callback != NULL)
    {
        s_state_sync_callback(reason, snapshot);
    }
}

static void apply_state_outputs(PowerState_t state)
{
    for (uint32_t io = 0; io < POWER_IO_COUNT; io++)
    {
        PowerIoLevel_t level = s_state_io_table[state][io];
        if (level != POWER_IO_LEVEL_KEEP)
        {
            PowerPort_WriteIo((PowerIoId_t)io, level);
        }
    }
}

static bool is_peripheral_count_valid(void)
{
    return s_config.peripheral_count <= POWER_MANAGER_MAX_PERIPHERALS;
}

static bool is_peripheral_active_in_state(const PowerPeripheralConfig_t *peripheral,
                                          PowerState_t state)
{
    PowerStateMask_t mask = (PowerStateMask_t)(1U << state);

    return (peripheral->active_state_mask & mask) != 0U;
}

static BaseType_t apply_one_peripheral_state(uint8_t index, PowerState_t state)
{
    const PowerPeripheralConfig_t *peripheral = &s_config.peripherals[index];
    bool should_be_active;
    BaseType_t result = pdPASS;

    if (!peripheral->enabled)
    {
        /* 单个外设调试开关关闭时跳过，不影响其它外设。 */
        return pdPASS;
    }

    should_be_active = is_peripheral_active_in_state(peripheral, state);
    if (should_be_active &&
        (s_peripheral_states[index] != POWER_PERIPHERAL_STATE_ACTIVE))
    {
        /* 目标状态需要外设工作，且当前未激活时才调用 resume，避免重复初始化。 */
        if (peripheral->resume != NULL)
        {
            result = peripheral->resume(peripheral, state);
        }
        if (result == pdPASS)
        {
            s_peripheral_states[index] = POWER_PERIPHERAL_STATE_ACTIVE;
        }
    }
    else if (!should_be_active &&
             (s_peripheral_states[index] != POWER_PERIPHERAL_STATE_SUSPENDED))
    {
        /* 目标状态不需要外设工作，且当前仍激活时才调用 suspend。 */
        if (peripheral->suspend != NULL)
        {
            result = peripheral->suspend(peripheral, state);
        }
        if (result == pdPASS)
        {
            s_peripheral_states[index] = POWER_PERIPHERAL_STATE_SUSPENDED;
        }
    }

    return result;
}

BaseType_t PowerManager_ApplyPeripheralState(PowerState_t state)
{
    BaseType_t result = pdPASS;

    if (!s_config.peripheral_pm_enabled || (s_config.peripherals == NULL))
    {
        /* 外设电源管理是可选功能，关闭时保持原状态机行为。 */
        return pdPASS;
    }

    if ((state > POWER_STATE_SHUTDOWN_PREPARE) || !is_peripheral_count_valid())
    {
        return pdFAIL;
    }

    for (uint8_t i = 0; i < s_config.peripheral_count; i++)
    {
        if (apply_one_peripheral_state(i, state) != pdPASS)
        {
            result = pdFAIL;
        }
    }

    return result;
}

static void set_state(PowerState_t next_state, PowerTransitionReason_t reason)
{
    PowerState_t old_state;
    PowerManagerSnapshot_t callback_snapshot;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    old_state = s_snapshot.state;
    if (old_state == next_state)
    {
        xSemaphoreGive(s_lock);
        return;
    }

    s_snapshot.state = next_state;
    s_snapshot.last_transition_reason = reason;
    s_state_enter_tick = xTaskGetTickCount();
    if (next_state == POWER_STATE_SHUTDOWN_PREPARE)
    {
        /* 每次进入关机准备态都重新收集各模块 ready，避免复用上一次关机结果。 */
        s_snapshot.shutdown_ready_mask = 0U;
    }
    callback_snapshot = s_snapshot;
    xSemaphoreGive(s_lock);

    apply_state_outputs(next_state);
    (void)PowerManager_ApplyPeripheralState(next_state);

    if (next_state == POWER_STATE_SLEEP)
    {
        PowerPort_PrepareMcuSleep();
    }
    else if (next_state == POWER_STATE_SHUTDOWN_PREPARE)
    {
        PowerPort_PrepareMcuShutdown();
    }

    if (s_state_callback != NULL)
    {
        s_state_callback(old_state, next_state, &callback_snapshot);
    }

    s_state_sync_cycle_count = 0;
    sync_power_state(POWER_STATE_SYNC_CHANGED, &callback_snapshot);
}

static void update_inputs(void)
{
    bool kl30_present = PowerPort_ReadKl30Present();
    bool kl15_on = PowerPort_ReadKl15On();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.kl30_present = kl30_present;
    s_snapshot.kl15_on = kl15_on;
    xSemaphoreGive(s_lock);
}

static PowerManagerSnapshot_t get_snapshot_locked_copy(void)
{
    PowerManagerSnapshot_t snapshot;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot = s_snapshot;
    xSemaphoreGive(s_lock);
    return snapshot;
}

static bool is_voltage_fault(PowerVoltageState_t state)
{
    return (state == POWER_VOLTAGE_STATE_SHUTDOWN_LOW) ||
           (state == POWER_VOLTAGE_STATE_OVER_VOLTAGE);
}

static bool is_voltage_valid_for_wakeup(PowerVoltageState_t state)
{
    return (state == POWER_VOLTAGE_STATE_WAKEUP_VALID) ||
           (state == POWER_VOLTAGE_STATE_WORK_VALID);
}

static bool is_voltage_valid_for_work(PowerVoltageState_t state)
{
    return state == POWER_VOLTAGE_STATE_WORK_VALID;
}

static PowerTransitionReason_t voltage_fault_reason(PowerVoltageState_t state)
{
    if (state == POWER_VOLTAGE_STATE_SHUTDOWN_LOW)
    {
        return POWER_TRANSITION_REASON_UNDER_VOLTAGE;
    }
    if (state == POWER_VOLTAGE_STATE_OVER_VOLTAGE)
    {
        return POWER_TRANSITION_REASON_OVER_VOLTAGE;
    }

    return POWER_TRANSITION_REASON_INVALID_STATE;
}

static bool is_shutdown_ready_snapshot(const PowerManagerSnapshot_t *snapshot)
{
    /* 所有 required bit 均被 ready bit 覆盖后才允许真正进入休眠。 */
    return (snapshot->shutdown_ready_mask & snapshot->shutdown_required_mask) ==
           snapshot->shutdown_required_mask;
}

static void run_state_machine(void)
{
    PowerManagerSnapshot_t snapshot = get_snapshot_locked_copy();
    TickType_t elapsed = xTaskGetTickCount() - s_state_enter_tick;

    if (!snapshot.kl30_present || is_voltage_fault(snapshot.voltage_state))
    {
        /* KL30 丢失、欠压、过压属于强制关机准备条件。 */
        PowerTransitionReason_t reason = !snapshot.kl30_present ?
                                         POWER_TRANSITION_REASON_KL30_LOST :
                                         voltage_fault_reason(snapshot.voltage_state);

        set_state(POWER_STATE_SHUTDOWN_PREPARE, reason);
        return;
    }

    switch (snapshot.state)
    {
    case POWER_STATE_SLEEP:
        if (snapshot.kl15_on && is_voltage_valid_for_wakeup(snapshot.voltage_state))
        {
            /* KL15 有效且电压已稳定到可唤醒范围，开始唤醒流程。 */
            set_state(POWER_STATE_WAKEUP, POWER_TRANSITION_REASON_KL15_ON);
        }
        break;

    case POWER_STATE_WAKEUP:
        if (!snapshot.kl15_on)
        {
            set_state(POWER_STATE_SHUTDOWN_PREPARE, POWER_TRANSITION_REASON_KL15_OFF);
        }
        else if ((elapsed >= s_config.wakeup_min_ticks) &&
                 is_voltage_valid_for_work(snapshot.voltage_state))
        {
            /* 唤醒保持时间到达，并且电压达到工作范围后进入正常工作。 */
            set_state(POWER_STATE_WORK, POWER_TRANSITION_REASON_WORK_VOLTAGE_VALID);
        }
        break;

    case POWER_STATE_WORK:
        if (!snapshot.kl15_on || !is_voltage_valid_for_wakeup(snapshot.voltage_state))
        {
            PowerTransitionReason_t reason = !snapshot.kl15_on ?
                                             POWER_TRANSITION_REASON_KL15_OFF :
                                             POWER_TRANSITION_REASON_UNDER_VOLTAGE;

            set_state(POWER_STATE_SHUTDOWN_PREPARE, reason);
        }
        break;

    case POWER_STATE_SHUTDOWN_PREPARE:
        if (snapshot.kl15_on && is_voltage_valid_for_wakeup(snapshot.voltage_state))
        {
            set_state(POWER_STATE_WAKEUP, POWER_TRANSITION_REASON_WAKEUP_VOLTAGE_VALID);
        }
        else if ((elapsed >= s_config.shutdown_prepare_ticks) &&
                 is_shutdown_ready_snapshot(&snapshot))
        {
            /* 关机准备时间到达，并且各模块确认完成后才进入休眠。 */
            set_state(POWER_STATE_SLEEP, POWER_TRANSITION_REASON_SHUTDOWN_READY);
        }
        else if (elapsed >= POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS)
        {
            /* 关机动作超时仍未完成时强制进入休眠，避免系统长时间停留在关机准备态。 */
            set_state(POWER_STATE_SLEEP, POWER_TRANSITION_REASON_SHUTDOWN_TIMEOUT_FORCE);
        }
        break;

    default:
        set_state(POWER_STATE_SHUTDOWN_PREPARE, POWER_TRANSITION_REASON_INVALID_STATE);
        break;
    }
}

static void update_voltage_snapshot(const PowerEvent_t *event)
{
    /* 电压状态由独立 VoltageMonitor task 发布，这里只更新快照供状态机消费。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.voltage_state = event->voltage_state;
    s_snapshot.vbatt_mv = event->voltage_mv;
    s_snapshot.over_voltage = event->voltage_state == POWER_VOLTAGE_STATE_OVER_VOLTAGE;
    s_snapshot.under_voltage = event->voltage_state == POWER_VOLTAGE_STATE_SHUTDOWN_LOW;
    xSemaphoreGive(s_lock);
}

static void pulse_restore_timer_callback(TimerHandle_t timer)
{
    PulseRestore_t *restore = (PulseRestore_t *)pvTimerGetTimerID(timer);
    if (restore != NULL)
    {
        PowerPort_WriteIo(restore->io, restore->restore_level);
        vPortFree(restore);
    }
    xTimerDelete(timer, 0);
}

static BaseType_t start_pulse(PowerIoId_t io,
                              PowerIoLevel_t active_level,
                              TickType_t duration_ticks)
{
    PulseRestore_t *restore;
    TimerHandle_t timer;

    if ((io >= POWER_IO_COUNT) ||
        (active_level == POWER_IO_LEVEL_KEEP) ||
        (duration_ticks == 0))
    {
        return pdFAIL;
    }

    restore = (PulseRestore_t *)pvPortMalloc(sizeof(*restore));
    if (restore == NULL)
    {
        return pdFAIL;
    }

    restore->io = io;
    restore->restore_level = PowerPort_ReadIo(io);
    /* 使用 FreeRTOS software timer 到期后恢复原电平，避免阻塞状态机任务。 */
    timer = xTimerCreate("pwrPulse",
                         duration_ticks,
                         pdFALSE,
                         restore,
                         pulse_restore_timer_callback);
    if (timer == NULL)
    {
        vPortFree(restore);
        return pdFAIL;
    }

    PowerPort_WriteIo(io, active_level);
    if (xTimerStart(timer, 0) != pdPASS)
    {
        PowerPort_WriteIo(io, restore->restore_level);
        xTimerDelete(timer, 0);
        vPortFree(restore);
        return pdFAIL;
    }

    return pdPASS;
}

static void handle_event(const PowerEvent_t *event)
{
    switch (event->type)
    {
    case POWER_EVENT_FORCE_SLEEP:
        set_state(POWER_STATE_SLEEP, POWER_TRANSITION_REASON_FORCE_REQUEST);
        break;
    case POWER_EVENT_FORCE_WAKEUP:
        set_state(POWER_STATE_WAKEUP, POWER_TRANSITION_REASON_FORCE_REQUEST);
        break;
    case POWER_EVENT_FORCE_WORK:
        set_state(POWER_STATE_WORK, POWER_TRANSITION_REASON_FORCE_REQUEST);
        break;
    case POWER_EVENT_FORCE_SHUTDOWN_PREPARE:
        set_state(POWER_STATE_SHUTDOWN_PREPARE, POWER_TRANSITION_REASON_FORCE_REQUEST);
        break;
    case POWER_EVENT_IO_PULSE:
        (void)start_pulse(event->io, event->active_level, event->duration_ticks);
        break;
    case POWER_EVENT_VOLTAGE_CHANGED:
        update_voltage_snapshot(event);
        break;
    default:
        break;
    }
}

static void power_manager_task(void *argument)
{
    PowerEvent_t event;
    (void)argument;

    apply_state_outputs(POWER_STATE_SLEEP);
    {
        PowerManagerSnapshot_t snapshot = get_snapshot_locked_copy();
        sync_power_state(POWER_STATE_SYNC_CHANGED, &snapshot);
    }

    for (;;)
    {
        while (xQueueReceive(s_event_queue, &event, 0) == pdPASS)
        {
            handle_event(&event);
        }

        update_inputs();
        run_state_machine();
        if (++s_state_sync_cycle_count >= s_config.state_sync_period_cycles)
        {
            /* 周期同步用于诊断/CAN/共享内存保活；状态变化会立即同步。 */
            PowerManagerSnapshot_t snapshot = get_snapshot_locked_copy();

            s_state_sync_cycle_count = 0;
            sync_power_state(POWER_STATE_SYNC_PERIODIC, &snapshot);
        }
        vTaskDelay(s_config.state_machine_period_ticks);
    }
}

BaseType_t PowerManager_Init(const PowerManagerConfig_t *config)
{
    if (s_event_queue != NULL)
    {
        return pdPASS;
    }

    if (!PowerPort_Init())
    {
        return pdFAIL;
    }

    if (config != NULL)
    {
        s_config = *config;
    }
    else
    {
        s_config = s_default_config;
    }

    if (s_config.state_machine_period_ticks == 0)
    {
        s_config.state_machine_period_ticks = s_default_config.state_machine_period_ticks;
    }
    if (s_config.state_sync_period_cycles == 0)
    {
        s_config.state_sync_period_cycles = s_default_config.state_sync_period_cycles;
    }
    if (!is_peripheral_count_valid())
    {
        return pdFAIL;
    }
    if (s_config.peripheral_pm_enabled &&
        (s_config.peripheral_count > 0U) &&
        (s_config.peripherals == NULL))
    {
        return pdFAIL;
    }

    s_lock = xSemaphoreCreateMutex();
    s_event_queue = xQueueCreate(POWER_MANAGER_QUEUE_LENGTH, sizeof(PowerEvent_t));
    if ((s_lock == NULL) || (s_event_queue == NULL))
    {
        if (s_lock != NULL)
        {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
        }
        if (s_event_queue != NULL)
        {
            vQueueDelete(s_event_queue);
            s_event_queue = NULL;
        }
        return pdFAIL;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = POWER_STATE_SLEEP;
    s_snapshot.last_transition_reason = POWER_TRANSITION_REASON_INIT;
    s_snapshot.voltage_state = POWER_VOLTAGE_STATE_UNKNOWN;
    s_snapshot.shutdown_required_mask = s_config.shutdown_required_mask;
    s_snapshot.shutdown_ready_mask = 0U;
    s_state_enter_tick = xTaskGetTickCount();
    s_state_sync_cycle_count = 0;
    for (uint8_t i = 0; i < POWER_MANAGER_MAX_PERIPHERALS; i++)
    {
        s_peripheral_states[i] = POWER_PERIPHERAL_STATE_SUSPENDED;
    }
    return pdPASS;
}

BaseType_t PowerManager_Start(UBaseType_t priority, uint16_t stack_words)
{
    if ((s_event_queue == NULL) || (s_task != NULL))
    {
        return pdFAIL;
    }

    return xTaskCreate(power_manager_task,
                       "pwrMgr",
                       stack_words,
                       NULL,
                       priority,
                       &s_task);
}

BaseType_t PowerManager_PostEvent(const PowerEvent_t *event, TickType_t timeout_ticks)
{
    if ((s_event_queue == NULL) || (event == NULL))
    {
        return pdFAIL;
    }

    return xQueueSend(s_event_queue, event, timeout_ticks);
}

BaseType_t PowerManager_PulseIo(PowerIoId_t io,
                                PowerIoLevel_t active_level,
                                TickType_t duration_ticks,
                                TickType_t timeout_ticks)
{
    PowerEvent_t event =
    {
        .type = POWER_EVENT_IO_PULSE,
        .io = io,
        .active_level = active_level,
        .duration_ticks = duration_ticks,
    };

    return PowerManager_PostEvent(&event, timeout_ticks);
}

PowerManagerSnapshot_t PowerManager_GetSnapshot(void)
{
    return get_snapshot_locked_copy();
}

void PowerManager_RegisterStateChangedCallback(PowerStateChangedCallback_t callback)
{
    s_state_callback = callback;
}

void PowerManager_RegisterStateSyncCallback(PowerStateSyncCallback_t callback)
{
    s_state_sync_callback = callback;
}

void PowerManager_SetShutdownReady(uint32_t module_mask, bool ready)
{
    if (s_lock == NULL)
    {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ready)
    {
        s_snapshot.shutdown_ready_mask |= module_mask;
    }
    else
    {
        s_snapshot.shutdown_ready_mask &= ~module_mask;
    }
    xSemaphoreGive(s_lock);
}

bool PowerManager_IsShutdownReady(void)
{
    if (s_lock == NULL)
    {
        return false;
    }

    PowerManagerSnapshot_t snapshot = get_snapshot_locked_copy();

    return is_shutdown_ready_snapshot(&snapshot);
}

uint32_t PowerManager_GetShutdownPendingMask(void)
{
    if (s_lock == NULL)
    {
        return 0U;
    }

    PowerManagerSnapshot_t snapshot = get_snapshot_locked_copy();

    return snapshot.shutdown_required_mask & ~snapshot.shutdown_ready_mask;
}

PowerPeripheralState_t PowerManager_GetPeripheralState(uint8_t peripheral_id)
{
    if (!is_peripheral_count_valid())
    {
        return POWER_PERIPHERAL_STATE_SUSPENDED;
    }

    for (uint8_t i = 0; i < s_config.peripheral_count; i++)
    {
        if (s_config.peripherals[i].id == peripheral_id)
        {
            return s_peripheral_states[i];
        }
    }

    return POWER_PERIPHERAL_STATE_SUSPENDED;
}

const char *PowerManager_StateName(PowerState_t state)
{
    switch (state)
    {
    case POWER_STATE_SLEEP:
        return "SLEEP";
    case POWER_STATE_WAKEUP:
        return "WAKEUP";
    case POWER_STATE_WORK:
        return "WORK";
    case POWER_STATE_SHUTDOWN_PREPARE:
        return "SHUTDOWN_PREPARE";
    default:
        return "UNKNOWN";
    }
}

const char *PowerManager_VoltageStateName(PowerVoltageState_t state)
{
    switch (state)
    {
    case POWER_VOLTAGE_STATE_UNKNOWN:
        return "UNKNOWN";
    case POWER_VOLTAGE_STATE_SHUTDOWN_LOW:
        return "SHUTDOWN_LOW";
    case POWER_VOLTAGE_STATE_LOW:
        return "LOW";
    case POWER_VOLTAGE_STATE_WAKEUP_VALID:
        return "WAKEUP_VALID";
    case POWER_VOLTAGE_STATE_WORK_VALID:
        return "WORK_VALID";
    case POWER_VOLTAGE_STATE_OVER_VOLTAGE:
        return "OVER_VOLTAGE";
    default:
        return "UNKNOWN";
    }
}

const char *PowerManager_TransitionReasonName(PowerTransitionReason_t reason)
{
    switch (reason)
    {
    case POWER_TRANSITION_REASON_INIT:
        return "INIT";
    case POWER_TRANSITION_REASON_KL30_LOST:
        return "KL30_LOST";
    case POWER_TRANSITION_REASON_KL15_ON:
        return "KL15_ON";
    case POWER_TRANSITION_REASON_KL15_OFF:
        return "KL15_OFF";
    case POWER_TRANSITION_REASON_UNDER_VOLTAGE:
        return "UNDER_VOLTAGE";
    case POWER_TRANSITION_REASON_OVER_VOLTAGE:
        return "OVER_VOLTAGE";
    case POWER_TRANSITION_REASON_WAKEUP_VOLTAGE_VALID:
        return "WAKEUP_VOLTAGE_VALID";
    case POWER_TRANSITION_REASON_WORK_VOLTAGE_VALID:
        return "WORK_VOLTAGE_VALID";
    case POWER_TRANSITION_REASON_SHUTDOWN_READY:
        return "SHUTDOWN_READY";
    case POWER_TRANSITION_REASON_SHUTDOWN_TIMEOUT_FORCE:
        return "SHUTDOWN_TIMEOUT_FORCE";
    case POWER_TRANSITION_REASON_FORCE_REQUEST:
        return "FORCE_REQUEST";
    case POWER_TRANSITION_REASON_INVALID_STATE:
        return "INVALID_STATE";
    default:
        return "UNKNOWN";
    }
}

const char *PowerManager_PeripheralTypeName(PowerPeripheralType_t type)
{
    switch (type)
    {
    case POWER_PERIPHERAL_TYPE_GPIO_POWER:
        return "GPIO_POWER";
    case POWER_PERIPHERAL_TYPE_CAN:
        return "CAN";
    case POWER_PERIPHERAL_TYPE_ADC:
        return "ADC";
    case POWER_PERIPHERAL_TYPE_UART:
        return "UART";
    case POWER_PERIPHERAL_TYPE_SPI:
        return "SPI";
    case POWER_PERIPHERAL_TYPE_I2C:
        return "I2C";
    case POWER_PERIPHERAL_TYPE_SENSOR:
        return "SENSOR";
    case POWER_PERIPHERAL_TYPE_STORAGE:
        return "STORAGE";
    case POWER_PERIPHERAL_TYPE_CUSTOM:
        return "CUSTOM";
    default:
        return "UNKNOWN";
    }
}
