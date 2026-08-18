#pragma once
#include <cstdint>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
extern "C" {
esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
esp_err_t nvs_get_i32(nvs_handle_t, const char*, int32_t*);
esp_err_t nvs_set_i32(nvs_handle_t, const char*, int32_t);
esp_err_t nvs_commit(nvs_handle_t); void nvs_close(nvs_handle_t);
}
