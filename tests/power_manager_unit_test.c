#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "power_manager.h"

static TickType_t g_tick;
static bool g_kl30_present;
static bool g_kl15_on;
static uint16_t g_battery_mv;
static uint32_t g_prepare_sleep_count;
static uint32_t g_prepare_shutdown_count;
static uint32_t g_resume_count;
static uint32_t g_suspend_count;
static int g_failures;

/*
 * Unit tests include the implementation file to exercise static state-machine
 * helpers. Production builds must compile src/power_manager.c normally instead.
 */
#include "../src/power_manager.c"

#define TEST_ASSERT(expr) test_assert((expr), #expr, __LINE__)

static void test_assert(bool ok, const char *expr, int line)
{
    if (!ok)
    {
        (void)expr;
        (void)line;
        g_failures++;
    }
}

void *pvPortMalloc(size_t size)
{
    return malloc(size);
}

void vPortFree(void *ptr)
{
    free(ptr);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    (void)semaphore;
    (void)ticks_to_wait;
    return pdPASS;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return pdPASS;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
}

QueueHandle_t xQueueCreate(UBaseType_t queue_length, UBaseType_t item_size)
{
    (void)queue_length;
    (void)item_size;
    return (QueueHandle_t)1;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    (void)queue;
    (void)item;
    (void)ticks_to_wait;
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t ticks_to_wait)
{
    (void)queue;
    (void)buffer;
    (void)ticks_to_wait;
    return pdFAIL;
}

void vQueueDelete(QueueHandle_t queue)
{
    (void)queue;
}

BaseType_t xTaskCreate(TaskFunction_t task_code,
                       const char *name,
                       uint16_t stack_depth,
                       void *parameters,
                       UBaseType_t priority,
                       TaskHandle_t *created_task)
{
    (void)task_code;
    (void)name;
    (void)stack_depth;
    (void)parameters;
    (void)priority;
    if (created_task != NULL)
    {
        *created_task = (TaskHandle_t)1;
    }
    return pdPASS;
}

TickType_t xTaskGetTickCount(void)
{
    return g_tick;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    g_tick += ticks_to_delay;
}

TimerHandle_t xTimerCreate(const char *name,
                           TickType_t period_ticks,
                           UBaseType_t auto_reload,
                           void *timer_id,
                           TimerCallbackFunction_t callback)
{
    (void)name;
    (void)period_ticks;
    (void)auto_reload;
    (void)timer_id;
    (void)callback;
    return (TimerHandle_t)1;
}

BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks_to_wait)
{
    (void)timer;
    (void)ticks_to_wait;
    return pdPASS;
}

BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks_to_wait)
{
    (void)timer;
    (void)ticks_to_wait;
    return pdPASS;
}

void *pvTimerGetTimerID(TimerHandle_t timer)
{
    (void)timer;
    return NULL;
}

bool PowerPort_Init(void)
{
    return true;
}

uint16_t PowerPort_ReadBatteryMv(void)
{
    return g_battery_mv;
}

bool PowerPort_ReadKl30Present(void)
{
    return g_kl30_present;
}

bool PowerPort_ReadKl15On(void)
{
    return g_kl15_on;
}

void PowerPort_WriteIo(PowerIoId_t io, PowerIoLevel_t level)
{
    (void)io;
    (void)level;
}

PowerIoLevel_t PowerPort_ReadIo(PowerIoId_t io)
{
    (void)io;
    return POWER_IO_LEVEL_LOW;
}

void PowerPort_PrepareMcuSleep(void)
{
    g_prepare_sleep_count++;
}

void PowerPort_PrepareMcuShutdown(void)
{
    g_prepare_shutdown_count++;
}

static BaseType_t test_peripheral_resume(const PowerPeripheralConfig_t *peripheral,
                                         PowerState_t power_state)
{
    (void)peripheral;
    (void)power_state;
    g_resume_count++;
    return pdPASS;
}

static BaseType_t test_peripheral_suspend(const PowerPeripheralConfig_t *peripheral,
                                          PowerState_t power_state)
{
    (void)peripheral;
    (void)power_state;
    g_suspend_count++;
    return pdPASS;
}

static void reset_runtime(void)
{
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_peripheral_states, 0, sizeof(s_peripheral_states));
    s_event_queue = NULL;
    s_lock = NULL;
    s_task = NULL;
    s_state_callback = NULL;
    s_state_sync_callback = NULL;
    s_state_enter_tick = 0;
    s_state_sync_cycle_count = 0;
    g_tick = 0;
    g_kl30_present = true;
    g_kl15_on = false;
    g_battery_mv = 12000;
    g_prepare_sleep_count = 0;
    g_prepare_shutdown_count = 0;
    g_resume_count = 0;
    g_suspend_count = 0;
}

static void init_power_manager(uint32_t shutdown_required_mask)
{
    PowerManagerConfig_t config =
    {
        .wakeup_min_ticks = pdMS_TO_TICKS(500),
        .shutdown_prepare_ticks = pdMS_TO_TICKS(2000),
        .state_machine_period_ticks = pdMS_TO_TICKS(50),
        .state_sync_period_cycles = 10,
        .shutdown_required_mask = shutdown_required_mask,
        .peripheral_pm_enabled = false,
        .peripherals = NULL,
        .peripheral_count = 0,
    };

    TEST_ASSERT(PowerManager_Init(&config) == pdPASS);
}

static void publish_voltage(PowerVoltageState_t state, uint16_t mv)
{
    PowerEvent_t event =
    {
        .type = POWER_EVENT_VOLTAGE_CHANGED,
        .voltage_state = state,
        .voltage_mv = mv,
    };

    handle_event(&event);
}

static void force_state(PowerEventType_t type)
{
    PowerEvent_t event =
    {
        .type = type,
    };

    handle_event(&event);
}

static void step_state_machine(void)
{
    update_inputs();
    run_state_machine();
}

static void test_sleep_to_wakeup_to_work(void)
{
    reset_runtime();
    init_power_manager(0U);

    g_kl15_on = true;
    publish_voltage(POWER_VOLTAGE_STATE_WAKEUP_VALID, 11200);
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_WAKEUP);
    TEST_ASSERT(PowerManager_GetSnapshot().last_transition_reason ==
                POWER_TRANSITION_REASON_KL15_ON);

    g_tick = pdMS_TO_TICKS(600);
    publish_voltage(POWER_VOLTAGE_STATE_WORK_VALID, 12200);
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_WORK);
    TEST_ASSERT(PowerManager_GetSnapshot().last_transition_reason ==
                POWER_TRANSITION_REASON_WORK_VOLTAGE_VALID);
}

static void test_work_to_shutdown_and_sleep_without_required_modules(void)
{
    reset_runtime();
    init_power_manager(0U);

    force_state(POWER_EVENT_FORCE_WORK);
    g_kl15_on = false;
    publish_voltage(POWER_VOLTAGE_STATE_WORK_VALID, 12000);
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_SHUTDOWN_PREPARE);
    TEST_ASSERT(PowerManager_GetSnapshot().last_transition_reason ==
                POWER_TRANSITION_REASON_KL15_OFF);

    g_tick += pdMS_TO_TICKS(2000);
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_SLEEP);
    TEST_ASSERT(PowerManager_GetSnapshot().last_transition_reason ==
                POWER_TRANSITION_REASON_SHUTDOWN_READY);
}

static void test_shutdown_waits_for_required_modules(void)
{
    reset_runtime();
    init_power_manager(POWER_SHUTDOWN_MODULE_CAN | POWER_SHUTDOWN_MODULE_APP);

    force_state(POWER_EVENT_FORCE_SHUTDOWN_PREPARE);
    g_kl15_on = false;
    publish_voltage(POWER_VOLTAGE_STATE_LOW, 10000);
    g_tick += pdMS_TO_TICKS(2000);
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_SHUTDOWN_PREPARE);
    TEST_ASSERT(PowerManager_GetShutdownPendingMask() ==
                (POWER_SHUTDOWN_MODULE_CAN | POWER_SHUTDOWN_MODULE_APP));

    PowerManager_SetShutdownReady(POWER_SHUTDOWN_MODULE_APP, true);
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_SHUTDOWN_PREPARE);
    TEST_ASSERT(PowerManager_GetShutdownPendingMask() == POWER_SHUTDOWN_MODULE_CAN);

    PowerManager_SetShutdownReady(POWER_SHUTDOWN_MODULE_CAN, true);
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_SLEEP);
}

static void test_shutdown_timeout_forces_sleep(void)
{
    reset_runtime();
    init_power_manager(POWER_SHUTDOWN_MODULE_APP);

    force_state(POWER_EVENT_FORCE_SHUTDOWN_PREPARE);
    g_kl15_on = false;
    publish_voltage(POWER_VOLTAGE_STATE_LOW, 10000);
    g_tick += POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS;
    step_state_machine();
    TEST_ASSERT(PowerManager_GetSnapshot().state == POWER_STATE_SLEEP);
    TEST_ASSERT(PowerManager_GetSnapshot().last_transition_reason ==
                POWER_TRANSITION_REASON_SHUTDOWN_TIMEOUT_FORCE);
}

static void test_peripheral_pm_resume_and_suspend(void)
{
    static const PowerPeripheralConfig_t peripherals[] =
    {
        {
            .id = 1,
            .type = POWER_PERIPHERAL_TYPE_CAN,
            .name = "CAN_TEST",
            .enabled = true,
            .active_state_mask = POWER_STATE_MASK_WORK,
            .resume = test_peripheral_resume,
            .suspend = test_peripheral_suspend,
            .user_context = NULL,
        },
    };
    PowerManagerConfig_t config =
    {
        .wakeup_min_ticks = pdMS_TO_TICKS(500),
        .shutdown_prepare_ticks = pdMS_TO_TICKS(2000),
        .state_machine_period_ticks = pdMS_TO_TICKS(50),
        .state_sync_period_cycles = 10,
        .shutdown_required_mask = 0U,
        .peripheral_pm_enabled = true,
        .peripherals = peripherals,
        .peripheral_count = 1,
    };

    reset_runtime();
    TEST_ASSERT(PowerManager_Init(&config) == pdPASS);

    force_state(POWER_EVENT_FORCE_WORK);
    TEST_ASSERT(g_resume_count == 1U);
    TEST_ASSERT(PowerManager_GetPeripheralState(1) == POWER_PERIPHERAL_STATE_ACTIVE);

    force_state(POWER_EVENT_FORCE_SLEEP);
    TEST_ASSERT(g_suspend_count == 1U);
    TEST_ASSERT(PowerManager_GetPeripheralState(1) == POWER_PERIPHERAL_STATE_SUSPENDED);
}

int main(void)
{
    test_sleep_to_wakeup_to_work();
    test_work_to_shutdown_and_sleep_without_required_modules();
    test_shutdown_waits_for_required_modules();
    test_shutdown_timeout_forces_sleep();
    test_peripheral_pm_resume_and_suspend();

    return g_failures;
}
