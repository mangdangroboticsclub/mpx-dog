#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102
#ifdef __cplusplus
extern "C"
#endif
const char *esp_err_to_name(esp_err_t);
