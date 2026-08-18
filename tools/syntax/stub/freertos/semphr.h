#pragma once
#include "freertos/FreeRTOS.h"
typedef void* SemaphoreHandle_t;
#ifdef __cplusplus
extern "C" {
#endif
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
#ifdef __cplusplus
}
#endif
