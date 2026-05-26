#ifndef TASK_H
#define TASK_H

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define tskIDLE_PRIORITY ((UBaseType_t)0)

BaseType_t xTaskCreate(TaskFunction_t task_code,
                       const char *name,
                       uint16_t stack_depth,
                       void *parameters,
                       UBaseType_t priority,
                       TaskHandle_t *created_task);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks_to_delay);

#endif
