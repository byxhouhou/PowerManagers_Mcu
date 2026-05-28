#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    POWER_STATE_SLEEP = 0,
    POWER_STATE_WAKEUP,
    POWER_STATE_WORK,
    POWER_STATE_SHUTDOWN_PREPARE,
} PowerState_t;

/* 逻辑输出 IO。实际管脚映射在 power_manager_port.c 中完成。 */
typedef enum
{
    POWER_IO_MAIN_RELAY = 0,
    POWER_IO_SENSOR_5V_EN,
    POWER_IO_CAN_STB,
    POWER_IO_MCU_HOLD,
    POWER_IO_STATUS_LED,
    POWER_IO_COUNT
} PowerIoId_t;

typedef enum
{
    POWER_IO_LEVEL_LOW = 0,
    POWER_IO_LEVEL_HIGH = 1,
    POWER_IO_LEVEL_KEEP = 2,
} PowerIoLevel_t;

typedef struct
{
    PowerState_t state;
    TickType_t delay_before_ticks;
    PowerIoId_t io;
    PowerIoLevel_t level;
} PowerIoSequenceStep_t;

/* 电压监控任务输出的稳定电压状态，状态机只消费这些状态，不直接处理 ADC 原始值。 */
typedef enum
{
    POWER_VOLTAGE_STATE_UNKNOWN = 0,
    POWER_VOLTAGE_STATE_SHUTDOWN_LOW,
    POWER_VOLTAGE_STATE_LOW,
    POWER_VOLTAGE_STATE_WAKEUP_VALID,
    POWER_VOLTAGE_STATE_WORK_VALID,
    POWER_VOLTAGE_STATE_OVER_VOLTAGE,
} PowerVoltageState_t;

/* 最近一次状态迁移原因，用于诊断、日志和状态同步。 */
typedef enum
{
    POWER_TRANSITION_REASON_INIT = 0,
    POWER_TRANSITION_REASON_KL30_LOST,
    POWER_TRANSITION_REASON_KL15_ON,
    POWER_TRANSITION_REASON_KL15_OFF,
    POWER_TRANSITION_REASON_UNDER_VOLTAGE,
    POWER_TRANSITION_REASON_OVER_VOLTAGE,
    POWER_TRANSITION_REASON_WAKEUP_VOLTAGE_VALID,
    POWER_TRANSITION_REASON_WORK_VOLTAGE_VALID,
    POWER_TRANSITION_REASON_SHUTDOWN_READY,
    POWER_TRANSITION_REASON_SHUTDOWN_TIMEOUT_FORCE,
    POWER_TRANSITION_REASON_FORCE_REQUEST,
    POWER_TRANSITION_REASON_INVALID_STATE,
} PowerTransitionReason_t;

/* 关机准备最大等待时间。超过该时间模块仍未 ready 时，状态机会强制进入休眠。 */
#ifndef POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS
#define POWER_MANAGER_SHUTDOWN_FORCE_TIMEOUT_TICKS pdMS_TO_TICKS(30000)
#endif

/* 日志编译开关。默认不编入格式化日志代码，减少 ROM/RAM 和 printf 依赖。 */
#ifndef POWER_MANAGER_LOG_ENABLED
#define POWER_MANAGER_LOG_ENABLED 0
#endif

/* 单条日志缓冲区长度，仅在 POWER_MANAGER_LOG_ENABLED != 0 时使用。 */
#ifndef POWER_MANAGER_LOG_BUFFER_SIZE
#define POWER_MANAGER_LOG_BUFFER_SIZE 128
#endif

typedef void (*PowerLogCallback_t)(const char *message);

/* 外设类型只表达类别，具体驱动动作由 resume/suspend 回调移植实现。 */
typedef enum
{
    POWER_PERIPHERAL_TYPE_GPIO_POWER = 0,
    POWER_PERIPHERAL_TYPE_CAN,
    POWER_PERIPHERAL_TYPE_ADC,
    POWER_PERIPHERAL_TYPE_UART,
    POWER_PERIPHERAL_TYPE_SPI,
    POWER_PERIPHERAL_TYPE_I2C,
    POWER_PERIPHERAL_TYPE_SENSOR,
    POWER_PERIPHERAL_TYPE_STORAGE,
    POWER_PERIPHERAL_TYPE_CUSTOM,
} PowerPeripheralType_t;

typedef enum
{
    POWER_PERIPHERAL_STATE_SUSPENDED = 0,
    POWER_PERIPHERAL_STATE_ACTIVE,
} PowerPeripheralState_t;

typedef uint8_t PowerStateMask_t;

/* 外设在哪些电源状态下保持 active，由 active_state_mask 配置。 */
#define POWER_STATE_MASK_SLEEP             ((PowerStateMask_t)(1U << POWER_STATE_SLEEP))
#define POWER_STATE_MASK_WAKEUP            ((PowerStateMask_t)(1U << POWER_STATE_WAKEUP))
#define POWER_STATE_MASK_WORK              ((PowerStateMask_t)(1U << POWER_STATE_WORK))
#define POWER_STATE_MASK_SHUTDOWN_PREPARE  ((PowerStateMask_t)(1U << POWER_STATE_SHUTDOWN_PREPARE))
#define POWER_STATE_MASK_ALL               ((PowerStateMask_t)(POWER_STATE_MASK_SLEEP | \
                                                               POWER_STATE_MASK_WAKEUP | \
                                                               POWER_STATE_MASK_WORK | \
                                                               POWER_STATE_MASK_SHUTDOWN_PREPARE))

struct PowerPeripheralConfig;
typedef BaseType_t (*PowerPeripheralAction_t)(const struct PowerPeripheralConfig *peripheral,
                                              PowerState_t power_state);

typedef struct PowerPeripheralConfig
{
    uint8_t id;
    PowerPeripheralType_t type;
    const char *name;
    bool enabled;
    PowerStateMask_t active_state_mask;
    PowerPeripheralAction_t resume;
    PowerPeripheralAction_t suspend;
    void *user_context;
} PowerPeripheralConfig_t;

/* 关机准备确认模块位。项目可继续扩展高位作为自定义模块。 */
#define POWER_SHUTDOWN_MODULE_CAN      (1UL << 0)
#define POWER_SHUTDOWN_MODULE_NVM      (1UL << 1)
#define POWER_SHUTDOWN_MODULE_DIAG     (1UL << 2)
#define POWER_SHUTDOWN_MODULE_APP      (1UL << 3)
#define POWER_SHUTDOWN_MODULE_NETWORK  (1UL << 4)

typedef struct
{
    uint16_t shutdown_mv;
    uint16_t wakeup_mv;
    uint16_t work_mv;
    uint16_t over_voltage_mv;
    uint16_t hysteresis_mv;
    uint8_t stable_sample_count;
} PowerVoltageThresholds_t;

typedef struct
{
    TickType_t wakeup_min_ticks;
    TickType_t shutdown_prepare_ticks;
    TickType_t state_machine_period_ticks;
    uint8_t state_sync_period_cycles;
    uint32_t shutdown_required_mask;
    bool io_sequence_enabled;
    const PowerIoSequenceStep_t *io_sequence_steps;
    uint8_t io_sequence_step_count;
    /* 外设电源管理总开关；关闭时不会调用任何外设 resume/suspend。 */
    bool peripheral_pm_enabled;
    const PowerPeripheralConfig_t *peripherals;
    uint8_t peripheral_count;
    /* 日志运行时开关。需同时打开 POWER_MANAGER_LOG_ENABLED 且设置 log_callback 才会输出。 */
    bool log_enabled;
    PowerLogCallback_t log_callback;
} PowerManagerConfig_t;

/* 对外同步的状态快照。回调中不要长期保存指针，应按值拷贝需要的数据。 */
typedef struct
{
    PowerState_t state;
    PowerTransitionReason_t last_transition_reason;
    PowerVoltageState_t voltage_state;
    uint16_t vbatt_mv;
    bool kl30_present;
    bool kl15_on;
    bool over_voltage;
    bool under_voltage;
    uint32_t shutdown_required_mask;
    uint32_t shutdown_ready_mask;
} PowerManagerSnapshot_t;

typedef enum
{
    POWER_EVENT_FORCE_SLEEP = 0,
    POWER_EVENT_FORCE_WAKEUP,
    POWER_EVENT_FORCE_WORK,
    POWER_EVENT_FORCE_SHUTDOWN_PREPARE,
    POWER_EVENT_IO_PULSE,
    POWER_EVENT_VOLTAGE_CHANGED,
} PowerEventType_t;

typedef struct
{
    PowerEventType_t type;
    PowerIoId_t io;
    PowerIoLevel_t active_level;
    TickType_t duration_ticks;
    PowerVoltageState_t voltage_state;
    uint16_t voltage_mv;
} PowerEvent_t;

typedef enum
{
    POWER_STATE_SYNC_CHANGED = 0,
    POWER_STATE_SYNC_PERIODIC,
} PowerStateSyncReason_t;

typedef void (*PowerStateChangedCallback_t)(PowerState_t old_state,
                                            PowerState_t new_state,
                                            const PowerManagerSnapshot_t *snapshot);

typedef void (*PowerStateSyncCallback_t)(PowerStateSyncReason_t reason,
                                         const PowerManagerSnapshot_t *snapshot);

BaseType_t PowerManager_Init(const PowerManagerConfig_t *config);
BaseType_t PowerManager_Start(UBaseType_t priority, uint16_t stack_words);
BaseType_t PowerManager_PostEvent(const PowerEvent_t *event, TickType_t timeout_ticks);
BaseType_t PowerManager_PulseIo(PowerIoId_t io,
                                PowerIoLevel_t active_level,
                                TickType_t duration_ticks,
                                TickType_t timeout_ticks);
PowerManagerSnapshot_t PowerManager_GetSnapshot(void);
void PowerManager_RegisterStateChangedCallback(PowerStateChangedCallback_t callback);
void PowerManager_RegisterStateSyncCallback(PowerStateSyncCallback_t callback);
void PowerManager_SetShutdownReady(uint32_t module_mask, bool ready);
bool PowerManager_IsShutdownReady(void);
uint32_t PowerManager_GetShutdownPendingMask(void);
BaseType_t PowerManager_ApplyPeripheralState(PowerState_t state);
PowerPeripheralState_t PowerManager_GetPeripheralState(uint8_t peripheral_id);

const char *PowerManager_StateName(PowerState_t state);
const char *PowerManager_VoltageStateName(PowerVoltageState_t state);
const char *PowerManager_TransitionReasonName(PowerTransitionReason_t reason);
const char *PowerManager_PeripheralTypeName(PowerPeripheralType_t type);

#ifdef __cplusplus
}
#endif

#endif
