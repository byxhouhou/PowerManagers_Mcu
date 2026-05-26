# FreeRTOS MCU 电源管理 / FreeRTOS MCU Power Manager

本模块实现一个 MCU 侧电源管理框架，采用“电压监控任务 + 电源状态机任务”的拆分方式。电压监控任务负责 ADC 采样、迟滞、防抖和稳定电压状态发布；电源状态机任务负责消费电压状态、KL30、KL15 和软件事件，并控制输出 IO、外设电源、关机确认和状态同步。

This module implements an MCU-side power management framework split into a voltage monitor task and a power state machine task. The voltage monitor handles ADC sampling, hysteresis, debounce, and stable voltage-state publishing. The power state machine consumes voltage state, KL30, KL15, and software events, then controls output IOs, peripheral power, shutdown confirmation, and state sync.

## 状态 / States

- `POWER_STATE_SLEEP`：休眠状态，关闭主要输出，CAN standby 置位，MCU hold 保持。  
  Sleep state. Main outputs are off, CAN standby is asserted, and MCU hold remains enabled.
- `POWER_STATE_WAKEUP`：唤醒状态，KL15 和电压有效后进入，逐步打开唤醒相关输出。  
  Wakeup state. Entered after KL15 and voltage are valid; wakeup-related outputs are enabled.
- `POWER_STATE_WORK`：正常工作状态，主继电器、传感器电源和通信外设进入工作。  
  Normal work state. Main relay, sensor supply, and communication peripherals are active.
- `POWER_STATE_SHUTDOWN_PREPARE`：关机准备状态，用于保存数据、停止通信、等待模块确认。  
  Shutdown preparation state. Used to save data, stop communication, and wait for module confirmations.

## 文件 / Files

- `include/power_manager.h`：电源状态机、事件、IO、外设、关机确认和同步接口。  
  Power state machine, events, IOs, peripherals, shutdown confirmation, and sync APIs.
- `include/power_voltage_monitor.h`：ADC 电压监控接口和阈值配置。  
  ADC voltage monitor API and threshold configuration.
- `include/power_manager_port.h`：板级适配接口。  
  Board adaptation interface.
- `src/power_manager.c`：FreeRTOS 电源状态机任务、KL 输入、IO 表、外设管理、脉冲输出。  
  FreeRTOS power state machine task, KL inputs, IO table, peripheral management, and pulse output.
- `src/power_voltage_monitor.c`：FreeRTOS 电压监控任务、ADC 采样、迟滞、防抖。  
  FreeRTOS voltage monitor task, ADC sampling, hysteresis, and debounce.
- `src/power_manager_port.c`：板级 stub，需要替换成真实 MCU HAL/SDK 实现。  
  Board-level stub to replace with real MCU HAL/SDK code.
- `examples/power_manager_app.c`：初始化、外设配置、同步回调和 IO 脉冲示例。  
  Initialization, peripheral configuration, sync callback, and IO pulse example.

## 集成 / Integration

1. 将 `src/power_manager.c` 和 `src/power_voltage_monitor.c` 加入固件工程。  
   Add `src/power_manager.c` and `src/power_voltage_monitor.c` to the firmware build.
2. 将 `src/power_manager_port.c` 中的 ADC/GPIO/KL30/KL15 stub 替换为真实板级实现。  
   Replace ADC/GPIO/KL30/KL15 stubs in `src/power_manager_port.c` with board-specific code.
3. 启用 FreeRTOS software timer。  
   Enable FreeRTOS software timers:
   - `configUSE_TIMERS == 1`
   - timer task and queue are configured.
4. 在调度器资源就绪后启动模块。  
   Start the modules after scheduler resources are ready.

```c
PowerManagerConfig_t cfg = {
    .wakeup_min_ticks = pdMS_TO_TICKS(500),
    .shutdown_prepare_ticks = pdMS_TO_TICKS(2000),
    .state_machine_period_ticks = pdMS_TO_TICKS(50),
    .state_sync_period_cycles = 10,
    .shutdown_required_mask = POWER_SHUTDOWN_MODULE_CAN |
                              POWER_SHUTDOWN_MODULE_NVM |
                              POWER_SHUTDOWN_MODULE_DIAG |
                              POWER_SHUTDOWN_MODULE_APP,
    .peripheral_pm_enabled = true,
    .peripherals = app_peripherals,
    .peripheral_count = app_peripheral_count,
};

PowerVoltageMonitorConfig_t voltage_cfg = {
    .sample_period_ticks = pdMS_TO_TICKS(20),
    .thresholds = {
        .shutdown_mv = 9000,
        .wakeup_mv = 10500,
        .work_mv = 11500,
        .over_voltage_mv = 16500,
        .hysteresis_mv = 500,
        .stable_sample_count = 5,
    },
};

(void)PowerManager_Init(&cfg);
(void)PowerVoltageMonitor_Init(&voltage_cfg);
(void)PowerManager_Start(tskIDLE_PRIORITY + 2, 512);
(void)PowerVoltageMonitor_Start(tskIDLE_PRIORITY + 3, 384);
```

## IO 脉冲 / IO Pulse

支持对指定 IO 拉出一个脉冲，到期后自动恢复原电平。  
The manager can pulse a specific IO and restore its previous level automatically.

```c
(void)PowerManager_PulseIo(POWER_IO_STATUS_LED,
                           POWER_IO_LEVEL_HIGH,
                           pdMS_TO_TICKS(100),
                           pdMS_TO_TICKS(10));
```

## 状态同步 / State Sync

注册 `PowerManager_RegisterStateSyncCallback()` 后，可以将当前电源快照同步给 CAN、诊断、共享内存或其它任务。  
Register `PowerManager_RegisterStateSyncCallback()` to synchronize the current power snapshot to CAN, diagnostics, shared memory, or another task.

- `POWER_STATE_SYNC_CHANGED`：电源状态变化后立即同步。  
  Synchronized immediately after a power-state transition.
- `POWER_STATE_SYNC_PERIODIC`：每 `state_sync_period_cycles` 个状态机周期同步一次，默认 `10`。  
  Synchronized every `state_sync_period_cycles` state-machine cycles, default `10`.

## 关机确认 / Shutdown Ready

`SHUTDOWN_PREPARE` 可以等待各业务模块完成关机动作后再进入 `SLEEP`。配置 `shutdown_required_mask` 后，各模块完成保存数据、停止通信、关闭驱动等动作时上报 ready。  
`SHUTDOWN_PREPARE` can wait for application modules to finish shutdown work before entering `SLEEP`. Configure `shutdown_required_mask`, then each module reports ready after saving data, stopping communication, and shutting down drivers.

```c
PowerManager_SetShutdownReady(POWER_SHUTDOWN_MODULE_CAN, true);
PowerManager_SetShutdownReady(POWER_SHUTDOWN_MODULE_NVM, true);
PowerManager_SetShutdownReady(POWER_SHUTDOWN_MODULE_DIAG, true);
PowerManager_SetShutdownReady(POWER_SHUTDOWN_MODULE_APP, true);
```

- `PowerManager_IsShutdownReady()`：所有 required 模块均 ready 时返回 true。  
  Returns true when all required modules are ready.
- `PowerManager_GetShutdownPendingMask()`：返回仍阻塞休眠的模块位。  
  Returns the module bits still blocking sleep.
- `shutdown_required_mask == 0` 时，保持简单默认行为：等待 `shutdown_prepare_ticks` 后即可休眠。  
  When `shutdown_required_mask == 0`, simple default behavior is kept: sleep is allowed after `shutdown_prepare_ticks`.
- 如果模块在 `POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS` 内仍未全部 ready，状态机会强制进入 `SLEEP`，默认超时时间为 30 秒。  
  If modules are still not ready within `POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS`, the state machine forces `SLEEP`; the default timeout is 30 seconds.

可在编译选项或公共配置头中覆盖该宏。  
Override this macro in compiler options or a common configuration header.

```c
#define POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS pdMS_TO_TICKS(30000)
```

## 外设电源管理 / Peripheral Power Management

外设管理是可选功能，由 `peripheral_pm_enabled` 总开关控制。每个外设还有独立 `.enabled` 开关，方便移植和调试时临时关闭单个外设管理。  
Peripheral management is optional and controlled by `peripheral_pm_enabled`. Each peripheral also has an independent `.enabled` switch, useful for porting and debugging.

```c
static const PowerPeripheralConfig_t app_peripherals[] = {
    {
        .id = 0,
        .type = POWER_PERIPHERAL_TYPE_CAN,
        .name = "CAN0",
        .enabled = true,
        .active_state_mask = POWER_STATE_MASK_WAKEUP | POWER_STATE_MASK_WORK,
        .resume = App_CanResume,
        .suspend = App_CanSuspend,
        .user_context = 0,
    },
};
```

- `.type`：外设类别，如 CAN、ADC、UART、SPI、I2C、SENSOR、STORAGE、CUSTOM。  
  Peripheral category, such as CAN, ADC, UART, SPI, I2C, SENSOR, STORAGE, or CUSTOM.
- `.active_state_mask`：配置外设在哪些电源状态下保持 active。  
  Configures the power states where the peripheral should be active.
- `.resume` / `.suspend`：板级回调，用于打开/关闭时钟、电源、收发器和驱动。  
  Board-specific callbacks for clocks, regulators, transceivers, and drivers.

状态机每次切换状态后会自动应用外设表。也可以手动调用 `PowerManager_ApplyPeripheralState(state)` 重新应用，并通过 `PowerManager_GetPeripheralState(id)` 做诊断。  
The state machine applies the peripheral table automatically after each state transition. You can also call `PowerManager_ApplyPeripheralState(state)` manually and use `PowerManager_GetPeripheralState(id)` for diagnostics.

## 单元测试 / Unit Tests

测试源码位于 `tests/power_manager_unit_test.c`，覆盖唤醒、工作、关机准备、关机 ready、30 秒强制关机和外设 resume/suspend。详细说明见 `tests/README.md`。  
The unit test source is `tests/power_manager_unit_test.c`. It covers wakeup, work, shutdown preparation, shutdown ready, 30-second forced shutdown, and peripheral resume/suspend. See `tests/README.md` for details.

## 状态决策摘要 / State Decision Summary

- `PowerVoltageMonitor` 负责 ADC 采样、电压分类、迟滞和防抖。  
  `PowerVoltageMonitor` owns ADC sampling, voltage classification, hysteresis, and debounce.
- 电压迁移使用 `hysteresis_mv`，默认 `500mV`。  
  Voltage transitions use `hysteresis_mv`, default `500mV`.
- 上升迁移要求 `voltage > threshold + hysteresis_mv`。  
  Upward transitions require `voltage > threshold + hysteresis_mv`.
- 下降迁移要求 `voltage < threshold - hysteresis_mv`。  
  Downward transitions require `voltage < threshold - hysteresis_mv`.
- 电压状态变化通过 `POWER_EVENT_VOLTAGE_CHANGED` 发给 `PowerManager`。  
  Voltage state changes are sent to `PowerManager` with `POWER_EVENT_VOLTAGE_CHANGED`.
- KL30 丢失、欠压、过压会强制进入 `SHUTDOWN_PREPARE`。  
  KL30 loss, under-voltage, or over-voltage forces `SHUTDOWN_PREPARE`.
- `SLEEP` 中 KL15 有效且电压达到唤醒范围后进入 `WAKEUP`。  
  In `SLEEP`, valid KL15 plus wakeup voltage enters `WAKEUP`.
- `WAKEUP` 中达到最小唤醒时间且电压达到工作范围后进入 `WORK`。  
  In `WAKEUP`, work voltage after the minimum wakeup time enters `WORK`.
- `WORK` 中 KL15 关闭或电压低于唤醒范围后进入 `SHUTDOWN_PREPARE`。  
  In `WORK`, KL15 OFF or voltage below wakeup range enters `SHUTDOWN_PREPARE`.
- `SHUTDOWN_PREPARE` 中 KL15 恢复且电压有效可回到 `WAKEUP`；否则在关机准备时间到达且模块 ready 后进入 `SLEEP`，若 30 秒超时仍未 ready 则强制进入 `SLEEP`。  
  In `SHUTDOWN_PREPARE`, valid KL15 and voltage return to `WAKEUP`; otherwise it enters `SLEEP` after shutdown time and ready confirmations. If modules are still not ready after the 30-second timeout, it forces `SLEEP`.
- 每个快照带有 `last_transition_reason`，用于诊断和同步。  
  Each snapshot carries `last_transition_reason` for diagnostics and sync.
