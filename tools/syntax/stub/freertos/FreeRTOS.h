#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int BaseType_t; typedef unsigned UBaseType_t; typedef uint32_t TickType_t;
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS 1
#define ESP_ERROR_CHECK(x) do{ (void)(x); }while(0)
