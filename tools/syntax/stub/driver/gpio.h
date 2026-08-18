#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT, GPIO_MODE_OUTPUT } gpio_mode_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define GPIO_INTR_DISABLE 0
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLUP_ENABLE 1
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t gpio_config(const gpio_config_t*);
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
int gpio_get_level(gpio_num_t);
#ifdef __cplusplus
}
#endif
