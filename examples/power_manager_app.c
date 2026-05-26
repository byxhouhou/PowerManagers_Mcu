#include "power_manager.h"
#include "power_voltage_monitor.h"

#include "FreeRTOS.h"
#include "task.h"

enum
{
    APP_PERIPHERAL_CAN = 0,
    APP_PERIPHERAL_ADC,
    APP_PERIPHERAL_NVM,
};

static BaseType_t app_peripheral_resume(const PowerPeripheralConfig_t *peripheral,
                                        PowerState_t power_state)
{
    (void)peripheral;
    (void)power_state;

    /*
     * Start clocks, regulators, transceivers, or drivers for this peripheral.
     */
    return pdPASS;
}

static BaseType_t app_peripheral_suspend(const PowerPeripheralConfig_t *peripheral,
                                         PowerState_t power_state)
{
    (void)peripheral;
    (void)power_state;

    /*
     * Stop drivers, place transceivers into standby, or gate peripheral clocks.
     */
    return pdPASS;
}

static const PowerPeripheralConfig_t s_peripherals[] =
{
    {
        .id = APP_PERIPHERAL_CAN,
        .type = POWER_PERIPHERAL_TYPE_CAN,
        .name = "CAN0",
        .enabled = true,
        .active_state_mask = POWER_STATE_MASK_WAKEUP |
                             POWER_STATE_MASK_WORK,
        .resume = app_peripheral_resume,
        .suspend = app_peripheral_suspend,
        .user_context = 0,
    },
    {
        .id = APP_PERIPHERAL_ADC,
        .type = POWER_PERIPHERAL_TYPE_ADC,
        .name = "ADC0",
        .enabled = true,
        .active_state_mask = POWER_STATE_MASK_ALL,
        .resume = app_peripheral_resume,
        .suspend = app_peripheral_suspend,
        .user_context = 0,
    },
    {
        .id = APP_PERIPHERAL_NVM,
        .type = POWER_PERIPHERAL_TYPE_STORAGE,
        .name = "NVM",
        .enabled = false,
        .active_state_mask = POWER_STATE_MASK_WORK |
                             POWER_STATE_MASK_SHUTDOWN_PREPARE,
        .resume = app_peripheral_resume,
        .suspend = app_peripheral_suspend,
        .user_context = 0,
    },
};

static void on_power_state_changed(PowerState_t old_state,
                                   PowerState_t new_state,
                                   const PowerManagerSnapshot_t *snapshot)
{
    (void)old_state;
    (void)new_state;
    (void)snapshot;

    /*
     * Notify diagnostics, CAN network management, or application tasks here.
     * When new_state is POWER_STATE_SHUTDOWN_PREPARE, these modules should
     * save data, stop communication, then call PowerManager_SetShutdownReady().
     */
}

static void on_power_state_sync(PowerStateSyncReason_t reason,
                                const PowerManagerSnapshot_t *snapshot)
{
    (void)reason;
    (void)snapshot;

    /*
     * Periodically publish power state to diagnostics/CAN/shared memory here.
     * This callback is also invoked immediately when power state changes.
     */
}

static void on_power_log(const char *message)
{
    (void)message;

    /*
     * Route to UART, RTT, CAN diagnostics, or platform logger here.
     * Enable with POWER_MANAGER_LOG_ENABLED=1 and config.log_enabled=true.
     */
}

void PowerManager_AppInit(void)
{
    PowerManagerConfig_t config =
    {
        .wakeup_min_ticks = pdMS_TO_TICKS(500),
        .shutdown_prepare_ticks = pdMS_TO_TICKS(2000),
        .state_machine_period_ticks = pdMS_TO_TICKS(50),
        .state_sync_period_cycles = 10,
        .shutdown_required_mask = POWER_SHUTDOWN_MODULE_CAN |
                                  POWER_SHUTDOWN_MODULE_NVM |
                                  POWER_SHUTDOWN_MODULE_DIAG |
                                  POWER_SHUTDOWN_MODULE_APP,
        .peripheral_pm_enabled = true,
        .peripherals = s_peripherals,
        .peripheral_count = sizeof(s_peripherals) / sizeof(s_peripherals[0]),
        .log_enabled = false,
        .log_callback = on_power_log,
    };
    PowerVoltageMonitorConfig_t voltage_config =
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

    (void)PowerManager_Init(&config);
    PowerManager_RegisterStateChangedCallback(on_power_state_changed);
    PowerManager_RegisterStateSyncCallback(on_power_state_sync);
    (void)PowerVoltageMonitor_Init(&voltage_config);
    (void)PowerManager_Start(tskIDLE_PRIORITY + 2, 512);
    (void)PowerVoltageMonitor_Start(tskIDLE_PRIORITY + 3, 384);
}

void PowerManager_AppPulseStatusLed(void)
{
    (void)PowerManager_PulseIo(POWER_IO_STATUS_LED,
                               POWER_IO_LEVEL_HIGH,
                               pdMS_TO_TICKS(100),
                               pdMS_TO_TICKS(10));
}

void PowerManager_AppReportShutdownReady(void)
{
    PowerManager_SetShutdownReady(POWER_SHUTDOWN_MODULE_APP, true);
}
