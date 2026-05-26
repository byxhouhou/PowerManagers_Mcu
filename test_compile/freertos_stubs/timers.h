#ifndef TIMERS_H
#define TIMERS_H

#include "FreeRTOS.h"

typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t timer);

TimerHandle_t xTimerCreate(const char *name,
                           TickType_t period_ticks,
                           UBaseType_t auto_reload,
                           void *timer_id,
                           TimerCallbackFunction_t callback);
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks_to_wait);
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks_to_wait);
void *pvTimerGetTimerID(TimerHandle_t timer);

#endif
