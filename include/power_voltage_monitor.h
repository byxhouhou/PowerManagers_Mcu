#ifndef POWER_VOLTAGE_MONITOR_H
#define POWER_VOLTAGE_MONITOR_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "power_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    TickType_t sample_period_ticks;
    PowerVoltageThresholds_t thresholds;
} PowerVoltageMonitorConfig_t;

BaseType_t PowerVoltageMonitor_Init(const PowerVoltageMonitorConfig_t *config);
BaseType_t PowerVoltageMonitor_Start(UBaseType_t priority, uint16_t stack_words);
PowerVoltageState_t PowerVoltageMonitor_GetState(void);
uint16_t PowerVoltageMonitor_GetVoltageMv(void);

#ifdef __cplusplus
}
#endif

#endif
