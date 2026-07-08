#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the Lua VM with PSRAM heap.
 *        Registers all built-in modules and robot bindings.
 *        Safe to call multiple times (idempotent — returns ESP_OK
 *        if already initialised).
 *
 * @return ESP_OK on success, ESP_FAIL if Lua state creation fails.
 */
esp_err_t lua_init(void);

/**
 * @brief Run a Lua script from a C string (synchronous).
 *        Blocks until the script completes or timeout expires.
 *
 * @param script      Lua source code (null-terminated)
 * @param output      Output buffer (stdout capture — written via print redirect)
 * @param output_size Size of output buffer
 * @param timeout_ms  Max execution time in ms (0 = no limit)
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if timeout reached,
 *         ESP_FAIL if Lua runtime error occurred
 */
esp_err_t lua_run_string(const char *script,
                         char *output, size_t output_size,
                         uint32_t timeout_ms);

/**
 * @brief Run a Lua script from a LittleFS file (synchronous).
 *
 * @param path        Absolute path in LittleFS (e.g. "/lua/test.lua")
 * @param output      Output buffer (stdout capture)
 * @param output_size Size of output buffer
 * @param timeout_ms  Max execution time in ms (0 = no limit)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file missing,
 *         ESP_ERR_TIMEOUT if timeout reached,
 *         ESP_FAIL if Lua runtime error occurred
 */
esp_err_t lua_run_file(const char *path,
                       char *output, size_t output_size,
                       uint32_t timeout_ms);

/**
 * @brief Get the current Lua VM state (for advanced use).
 *        Returns NULL if lua_init() has not been called.
 */
struct lua_State *lua_get_state(void);

/**
 * @brief De-initialise the Lua VM and free all resources.
 *        Safe to call even if not initialised.
 */
void lua_deinit(void);

#ifdef __cplusplus
}
#endif
