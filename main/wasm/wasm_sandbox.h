#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace wasm {

/**
 * @brief Result of a sandbox execution attempt.
 */
enum class SandboxResult {
	Success,
	LoadFailed,
	InstantiateFailed,
	FunctionNotFound,
	ExecutionFailed,
	Timeout,
	NotInitialized,
};

/**
 * @brief Initialize the WAMR runtime.
 *
 * Allocates a fixed heap pool in PSRAM for the runtime.
 * Must be called once before any sandbox operations.
 *
 * @return true on success.
 */
bool init_sandbox();

/**
 * @brief Register a set of native host functions with the runtime.
 *
 * Can be called any time after init_sandbox() and before
 * the first load_and_run(). Multiple registrations are additive.
 *
 * @param module_name  The import module name the wasm uses (e.g. "env").
 * @param symbols      Array of NativeSymbol entries.
 * @param count        Number of symbols in the array.
 * @return true on success.
 */
bool register_natives(const char *module_name,
					  void *symbols, std::uint32_t count);

/**
 * @brief Load a .wasm file from LittleFS, instantiate it, and call
 *        an exported function.
 *
 * If func_name is nullptr or empty, calls the default "_start" entry.
 * The instance is destroyed after the call returns.
 *
 * @param path       Path within LittleFS (e.g. "/skill.wasm").
 * @param func_name  Name of the exported function to call.
 * @param timeout_ms Maximum execution time in milliseconds (0 = no watchdog).
 * @return SandboxResult indicating the outcome.
 */
SandboxResult load_and_run(const char *path,
						   const char *func_name = nullptr,
						   std::uint32_t timeout_ms = 3000);

/**
 * @brief Load a .wasm module from an in-memory buffer, instantiate it,
 *        call an exported function, then tear down.
 *
 * Identical to load_and_run() but takes a pointer + size instead of a
 * LittleFS path. Useful for embedded binaries.
 *
 * @param wasm_bytes  Pointer to the .wasm bytecode in memory.
 * @param wasm_size   Number of bytes.
 * @param func_name   Exported function to call (default: "on_start").
 * @param timeout_ms  Watchdog timeout (default: 3000).
 * @return SandboxResult
 */
SandboxResult load_and_run_bytes(const uint8_t *wasm_bytes,
								 std::size_t wasm_size,
								 const char *func_name = nullptr,
								 std::uint32_t timeout_ms = 3000);

/**
 * @brief Check whether the currently running WASM invocation has been
 *        cancelled by the watchdog timer.
 *
 * Long-running host functions (e.g. robot_delay_ms) should call this
 * periodically and return early if true, allowing the WASM thread to
 * terminate promptly after a watchdog timeout.
 */
bool was_cancelled();

/**
 * @brief Check whether a WASM skill is currently executing.
 *
 * Used by the gait task to avoid flushing neutral positions while a
 * skill is running, preventing the gait task from overwriting the
 * skill's servo commands.
 */
bool is_running();

/**
 * @brief Destroy the WAMR runtime and free all resources.
 */
void destroy_sandbox();

}  // namespace wasm
