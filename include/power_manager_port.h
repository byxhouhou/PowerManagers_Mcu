#ifndef POWER_MANAGER_PORT_H
#define POWER_MANAGER_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "power_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

bool PowerPort_Init(void);
uint16_t PowerPort_ReadBatteryMv(void);
bool PowerPort_ReadKl30Present(void);
bool PowerPort_ReadKl15On(void);
void PowerPort_WriteIo(PowerIoId_t io, PowerIoLevel_t level);
PowerIoLevel_t PowerPort_ReadIo(PowerIoId_t io);
void PowerPort_PrepareMcuSleep(void);
void PowerPort_PrepareMcuShutdown(void);

#ifdef __cplusplus
}
#endif

#endif
