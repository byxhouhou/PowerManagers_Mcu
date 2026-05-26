#include "power_voltage_monitor.h"
#include "power_manager_port.h"

#include "semphr.h"
#include "task.h"

static const PowerVoltageMonitorConfig_t s_default_config =
{
    .sample_period_ticks = pdMS_TO_TICKS(20),
    .thresholds =
    {
        .shutdown_mv = 9000,
        .wakeup_mv = 10500,
        .work_mv = 11500,
        .over_voltage_mv = 16500,
        .hysteresis_mv = 500,
        .stable_sample_count = 5,
    },
};

static PowerVoltageMonitorConfig_t s_config;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static PowerVoltageState_t s_stable_state = POWER_VOLTAGE_STATE_UNKNOWN;
static PowerVoltageState_t s_pending_state = POWER_VOLTAGE_STATE_UNKNOWN;
static uint16_t s_voltage_mv;
static uint8_t s_pending_count;

static bool voltage_exceeds(uint16_t mv, uint16_t threshold_mv)
{
    /* 上升迁移必须超过 threshold + hysteresis，避免阈值附近来回抖动。 */
    uint32_t high_limit_mv = (uint32_t)threshold_mv +
                             (uint32_t)s_config.thresholds.hysteresis_mv;

    return (uint32_t)mv > high_limit_mv;
}

static bool voltage_below(uint16_t mv, uint16_t threshold_mv)
{
    /* 下降迁移必须低于 threshold - hysteresis，与上升迁移形成迟滞窗口。 */
    uint16_t hysteresis_mv = s_config.thresholds.hysteresis_mv;
    uint16_t low_limit_mv = (threshold_mv > hysteresis_mv) ?
                            (uint16_t)(threshold_mv - hysteresis_mv) :
                            0U;

    return mv < low_limit_mv;
}

static PowerVoltageState_t classify_nominal_voltage(uint16_t mv)
{
    /* 初始分类不使用迟滞，先给出当前电压所在的基础区间。 */
    if (mv >= s_config.thresholds.over_voltage_mv)
    {
        return POWER_VOLTAGE_STATE_OVER_VOLTAGE;
    }
    if (mv >= s_config.thresholds.work_mv)
    {
        return POWER_VOLTAGE_STATE_WORK_VALID;
    }
    if (mv >= s_config.thresholds.wakeup_mv)
    {
        return POWER_VOLTAGE_STATE_WAKEUP_VALID;
    }
    if (mv < s_config.thresholds.shutdown_mv)
    {
        return POWER_VOLTAGE_STATE_SHUTDOWN_LOW;
    }

    return POWER_VOLTAGE_STATE_LOW;
}

static PowerVoltageState_t classify_rising_voltage(uint16_t mv)
{
    if (voltage_exceeds(mv, s_config.thresholds.over_voltage_mv))
    {
        return POWER_VOLTAGE_STATE_OVER_VOLTAGE;
    }
    if (voltage_exceeds(mv, s_config.thresholds.work_mv))
    {
        return POWER_VOLTAGE_STATE_WORK_VALID;
    }
    if (voltage_exceeds(mv, s_config.thresholds.wakeup_mv))
    {
        return POWER_VOLTAGE_STATE_WAKEUP_VALID;
    }
    if (voltage_exceeds(mv, s_config.thresholds.shutdown_mv))
    {
        return POWER_VOLTAGE_STATE_LOW;
    }

    return POWER_VOLTAGE_STATE_SHUTDOWN_LOW;
}

static PowerVoltageState_t classify_with_hysteresis(PowerVoltageState_t current,
                                                    uint16_t mv)
{
    if (current == POWER_VOLTAGE_STATE_UNKNOWN)
    {
        return classify_nominal_voltage(mv);
    }

    if (current == POWER_VOLTAGE_STATE_OVER_VOLTAGE)
    {
        /* 过压恢复必须低于过压阈值减迟滞，避免过压边界抖动。 */
        if (voltage_below(mv, s_config.thresholds.over_voltage_mv))
        {
            return classify_nominal_voltage(mv);
        }
        return POWER_VOLTAGE_STATE_OVER_VOLTAGE;
    }

    if (voltage_exceeds(mv, s_config.thresholds.over_voltage_mv))
    {
        return POWER_VOLTAGE_STATE_OVER_VOLTAGE;
    }

    switch (current)
    {
    case POWER_VOLTAGE_STATE_SHUTDOWN_LOW:
        return classify_rising_voltage(mv);

    case POWER_VOLTAGE_STATE_LOW:
        if (voltage_below(mv, s_config.thresholds.shutdown_mv))
        {
            return POWER_VOLTAGE_STATE_SHUTDOWN_LOW;
        }
        if (voltage_exceeds(mv, s_config.thresholds.wakeup_mv))
        {
            return classify_rising_voltage(mv);
        }
        return POWER_VOLTAGE_STATE_LOW;

    case POWER_VOLTAGE_STATE_WAKEUP_VALID:
        if (voltage_below(mv, s_config.thresholds.wakeup_mv))
        {
            return voltage_below(mv, s_config.thresholds.shutdown_mv) ?
                   POWER_VOLTAGE_STATE_SHUTDOWN_LOW :
                   POWER_VOLTAGE_STATE_LOW;
        }
        if (voltage_exceeds(mv, s_config.thresholds.work_mv))
        {
            return POWER_VOLTAGE_STATE_WORK_VALID;
        }
        return POWER_VOLTAGE_STATE_WAKEUP_VALID;

    case POWER_VOLTAGE_STATE_WORK_VALID:
        if (voltage_below(mv, s_config.thresholds.work_mv))
        {
            return voltage_below(mv, s_config.thresholds.wakeup_mv) ?
                   POWER_VOLTAGE_STATE_LOW :
                   POWER_VOLTAGE_STATE_WAKEUP_VALID;
        }
        return POWER_VOLTAGE_STATE_WORK_VALID;

    default:
        return classify_nominal_voltage(mv);
    }
}

static void publish_voltage_state(PowerVoltageState_t state, uint16_t mv)
{
    /* 电压监控只发布稳定后的电压状态，状态机不直接读取 ADC 原始值。 */
    PowerEvent_t event =
    {
        .type = POWER_EVENT_VOLTAGE_CHANGED,
        .voltage_state = state,
        .voltage_mv = mv,
    };

    (void)PowerManager_PostEvent(&event, 0);
}

static void update_stable_voltage_state(PowerVoltageState_t candidate,
                                        uint16_t mv)
{
    if (candidate == s_stable_state)
    {
        s_pending_state = candidate;
        s_pending_count = 0;
        return;
    }

    if (candidate != s_pending_state)
    {
        /* 候选状态变化时重新计数，必须连续稳定若干次才发布。 */
        s_pending_state = candidate;
        s_pending_count = 1;
    }
    else if (s_pending_count < UINT8_MAX)
    {
        s_pending_count++;
    }

    if (s_pending_count >= s_config.thresholds.stable_sample_count)
    {
        /* 防抖完成：更新稳定状态并通知电源状态机。 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_stable_state = candidate;
        s_voltage_mv = mv;
        xSemaphoreGive(s_lock);

        s_pending_count = 0;
        publish_voltage_state(candidate, mv);
    }
}

static void power_voltage_monitor_task(void *argument)
{
    (void)argument;

    for (;;)
    {
        /* ADC 采样、迟滞分类、防抖发布全部在独立任务中完成。 */
        uint16_t mv = PowerPort_ReadBatteryMv();
        PowerVoltageState_t current = PowerVoltageMonitor_GetState();
        PowerVoltageState_t candidate = classify_with_hysteresis(current, mv);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_voltage_mv = mv;
        xSemaphoreGive(s_lock);

        update_stable_voltage_state(candidate, mv);
        vTaskDelay(s_config.sample_period_ticks);
    }
}

BaseType_t PowerVoltageMonitor_Init(const PowerVoltageMonitorConfig_t *config)
{
    if (s_lock != NULL)
    {
        return pdPASS;
    }

    if (config != NULL)
    {
        s_config = *config;
    }
    else
    {
        s_config = s_default_config;
    }

    if (s_config.sample_period_ticks == 0)
    {
        s_config.sample_period_ticks = s_default_config.sample_period_ticks;
    }
    if (s_config.thresholds.stable_sample_count == 0)
    {
        s_config.thresholds.stable_sample_count = 1;
    }
    if (s_config.thresholds.hysteresis_mv == 0)
    {
        s_config.thresholds.hysteresis_mv = s_default_config.thresholds.hysteresis_mv;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        return pdFAIL;
    }

    s_stable_state = POWER_VOLTAGE_STATE_UNKNOWN;
    s_pending_state = POWER_VOLTAGE_STATE_UNKNOWN;
    s_pending_count = 0;
    s_voltage_mv = 0;
    return pdPASS;
}

BaseType_t PowerVoltageMonitor_Start(UBaseType_t priority, uint16_t stack_words)
{
    if ((s_lock == NULL) || (s_task != NULL))
    {
        return pdFAIL;
    }

    return xTaskCreate(power_voltage_monitor_task,
                       "voltMon",
                       stack_words,
                       NULL,
                       priority,
                       &s_task);
}

PowerVoltageState_t PowerVoltageMonitor_GetState(void)
{
    PowerVoltageState_t state;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    state = s_stable_state;
    xSemaphoreGive(s_lock);

    return state;
}

uint16_t PowerVoltageMonitor_GetVoltageMv(void)
{
    uint16_t mv;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mv = s_voltage_mv;
    xSemaphoreGive(s_lock);

    return mv;
}
