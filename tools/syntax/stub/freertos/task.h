#pragma once
#include "freertos/FreeRTOS.h"
typedef void* TaskHandle_t;
#ifdef __cplusplus
extern "C" {
#endif
BaseType_t xTaskCreate(void (*)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*);
BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*, int);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
#ifdef __cplusplus
}
#endif
