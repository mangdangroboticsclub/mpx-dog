#include "wasm/wasm_sandbox.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <pthread.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

#include "fs/littlefs_manager.h"

static const char *TAG = "wasm_sandbox";

// ── Cooperative cancellation flag ────────────────────────────
// Set by the watchdog polling loop when the timeout expires.
// Long-running host functions check this via was_cancelled()
// and return early, allowing the WASM thread to terminate.
static std::atomic<bool> s_cancelled{false};

// ── WASM execution active flag ──────────────────────────────
// Set while a WASM module is executing.  The gait task checks
// this to avoid flushing neutral positions that would overwrite
// the skill's servo commands.
static std::atomic<bool> s_running{false};

// --- Resource budgets -------------------------------------------------------
// The runtime heap: used by WAMR for internal data structures.
// Allocated in PSRAM to conserve internal DRAM for WiFi/TCP.
static constexpr std::size_t RUNTIME_HEAP_SIZE = 128 * 1024;

// Linear memory limits per wasm instance (REQ-ROB-03: ≤128 KB).
static constexpr std::size_t DEFAULT_STACK_SIZE = 8 * 1024;   // 8 KB wasm stack
static constexpr std::size_t DEFAULT_HEAP_SIZE = 128 * 1024;  // 128 KB host-managed heap

// --- Global state -----------------------------------------------------------
static bool s_initialized = false;

// ── PSRAM-aware allocators (matching esp-wasmachine approach) ──────────────

static void *wamr_malloc(unsigned int size)
{
#ifdef CONFIG_SPIRAM
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    uint32_t caps = MALLOC_CAP_8BIT;
#endif
    return heap_caps_aligned_alloc(8, size, caps);
}

static void *wamr_realloc(void *ptr, unsigned int size)
{
    void *new_ptr = wamr_malloc(size);
    if (new_ptr && ptr) {
        size_t old_size = heap_caps_get_allocated_size(ptr);
        memcpy(new_ptr, ptr, old_size < size ? old_size : size);
        heap_caps_free(ptr);
    }
    return new_ptr;
}

static void wamr_free(void *ptr)
{
    heap_caps_free(ptr);
}

// --- Forward declaration from the SDK host functions module -----------------
extern "C" void wasm_host_functions_register();

// ── Struct for passing args to the pthread runner ─────────────────────────
struct LoadRunArgs {
	const uint8_t *wasm_bytes;
	std::size_t wasm_size;
	const char *func_name;
	uint32_t timeout_ms;
	volatile bool completed;
	wasm::SandboxResult result;

	// Shared module instance handle — set by the pthread after
	// instantiation, so the watchdog thread can call
	// wasm_runtime_terminate() to forcibly terminate an infinite
	// WASM loop.
	wasm_module_inst_t module_inst;
};

/**
 * @brief Run the full wasm lifecycle in a pthread.
 *
 * WAMR's ESP-IDF platform layer uses pthread_self() internally, so ALL
 * operations that touch WAMR (load, instantiate, lookup, execute) MUST
 * run in a thread created via pthread_create, not a bare xTaskCreate'd
 * FreeRTOS task.
 */
static void *wasm_load_run_thread(void *arg)
{
	auto *args = static_cast<LoadRunArgs *>(arg);

	char error_buf[256];

	// 2. Load the module
	wasm_module_t module = wasm_runtime_load(
		const_cast<uint8_t *>(args->wasm_bytes),
		static_cast<uint32_t>(args->wasm_size),
		error_buf, sizeof(error_buf));

	if (!module) {
		ESP_LOGE(TAG, "WAMR load failed: %s", error_buf);
		args->result = wasm::SandboxResult::LoadFailed;
		args->completed = true;
		return nullptr;
	}

	// 3. Instantiate
	const char *func_name = args->func_name;
	if (!func_name || func_name[0] == '\0') {
		func_name = "on_start";
	}

	wasm_module_inst_t inst = wasm_runtime_instantiate(
		module,
		DEFAULT_STACK_SIZE,
		DEFAULT_HEAP_SIZE,
		error_buf, sizeof(error_buf));

	if (!inst) {
		ESP_LOGE(TAG, "WAMR instantiate failed: %s", error_buf);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::InstantiateFailed;
		args->completed = true;
		return nullptr;
	}

	// Share the instance handle so the watchdog can terminate us
	args->module_inst = inst;

	ESP_LOGI(TAG, "Module instantiated (stack=%zuKB, linear_mem=%zuKB)",
			 DEFAULT_STACK_SIZE / 1024, DEFAULT_HEAP_SIZE / 1024);

	// 4. Look up the exported function
	wasm_function_inst_t func = wasm_runtime_lookup_function(inst, func_name, nullptr);
	if (!func) {
		func = wasm_runtime_lookup_function(inst, "_start", nullptr);
	}
	if (!func) {
		ESP_LOGW(TAG, "No exported function '%s' or '_start' found", func_name);
		args->module_inst = nullptr;
		wasm_runtime_deinstantiate(inst);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::FunctionNotFound;
		args->completed = true;
		return nullptr;
	}

	// 5. Create exec env and call the function
	wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst,
															DEFAULT_STACK_SIZE);
	if (!exec_env) {
		ESP_LOGE(TAG, "Failed to create exec env");
		args->module_inst = nullptr;
		wasm_runtime_deinstantiate(inst);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::ExecutionFailed;
		args->completed = true;
		return nullptr;
	}

	uint32_t argv[1] = {0};

	s_running = true;
	bool exec_ok = wasm_runtime_call_wasm(exec_env, func, 0, argv);
	s_running = false;

	wasm_runtime_destroy_exec_env(exec_env);

	if (!exec_ok) {
		const char *exc = wasm_runtime_get_exception(inst);
		ESP_LOGE(TAG, "WASM execution failed: %s",
				 exc ? exc : "unknown error");
		args->module_inst = nullptr;
		wasm_runtime_deinstantiate(inst);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::ExecutionFailed;
		args->completed = true;
		return nullptr;
	}

	ESP_LOGI(TAG, "WASM function '%s' completed successfully", func_name);

	// 6. Clean up
	args->module_inst = nullptr;
	wasm_runtime_deinstantiate(inst);
	wasm_runtime_unload(module);

	args->result = wasm::SandboxResult::Success;
	args->completed = true;
	return nullptr;
}

// ============================================================================
//  Public API
// ============================================================================

namespace wasm {

bool init_sandbox()
{
	if (s_initialized) {
		ESP_LOGW(TAG, "Sandbox already initialised");
		return true;
	}

	RuntimeInitArgs init_args;
	memset(&init_args, 0, sizeof(init_args));

	init_args.mem_alloc_type = Alloc_With_Allocator;
	init_args.mem_alloc_option.allocator.malloc_func = (void *)wamr_malloc;
	init_args.mem_alloc_option.allocator.realloc_func = (void *)wamr_realloc;
	init_args.mem_alloc_option.allocator.free_func = (void *)wamr_free;

	if (!wasm_runtime_full_init(&init_args)) {
		ESP_LOGE(TAG, "WAMR runtime initialisation failed");
		return false;
	}

	s_initialized = true;

	// Register host functions defined in the SDK module
	wasm_host_functions_register();

	ESP_LOGI(TAG, "WAMR sandbox initialised (mode=interp, allocator=psram)");

	return true;
}

bool register_natives(const char *module_name,
					  void *symbols, uint32_t count)
{
	if (!s_initialized) {
		ESP_LOGE(TAG, "Cannot register natives: sandbox not initialised");
		return false;
	}

	bool ok = wasm_runtime_register_natives(
		module_name,
		static_cast<NativeSymbol *>(symbols),
		count);

	if (!ok) {
		ESP_LOGE(TAG, "Failed to register %" PRIu32 " native(s) in module '%s'",
				 count, module_name);
	} else {
		ESP_LOGI(TAG, "Registered %" PRIu32 " native(s) in module '%s'",
				 count, module_name);
	}

	return ok;
}

SandboxResult load_and_run(const char *path,
						   const char *func_name,
						   uint32_t timeout_ms)
{
	if (!s_initialized) {
		ESP_LOGE(TAG, "Sandbox not initialised");
		return SandboxResult::NotInitialized;
	}

	// Read .wasm from LittleFS
	auto wasm_bytes = fs::read_file(path);
	if (wasm_bytes.empty()) {
		ESP_LOGE(TAG, "Failed to read '%s' from LittleFS", path);
		return SandboxResult::LoadFailed;
	}

	ESP_LOGI(TAG, "Read %zu bytes from '%s'", wasm_bytes.size(), path);

	return load_and_run_bytes(wasm_bytes.data(), wasm_bytes.size(),
							  func_name, timeout_ms);
}

SandboxResult load_and_run_bytes(const uint8_t *wasm_bytes,
								 std::size_t wasm_size,
								 const char *func_name,
								 uint32_t timeout_ms)
{
	if (!s_initialized) {
		ESP_LOGE(TAG, "Sandbox not initialised");
		return SandboxResult::NotInitialized;
	}

	// Everything that touches WAMR (load, instantiate, lookup, execute)
	// must run in a pthread, not a FreeRTOS task, because WAMR's ESP-IDF
	// platform layer calls pthread_self() internally.
	pthread_t thread;
	pthread_attr_t attr;
	LoadRunArgs args = { wasm_bytes, wasm_size, func_name, timeout_ms,
						 false, SandboxResult::NotInitialized, nullptr };

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, DEFAULT_STACK_SIZE + 4096);

	int ret = pthread_create(&thread, &attr, wasm_load_run_thread, &args);
	pthread_attr_destroy(&attr);

	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to create loader thread: %d", ret);
		return SandboxResult::ExecutionFailed;
	}

	// Wait with polling timeout (ESP-IDF lacks timedjoin/tryjoin)
	if (timeout_ms > 0) {
		const int poll_ms = 10;
		int elapsed = 0;
		while (elapsed < static_cast<int>(timeout_ms) && !args.completed) {
			vTaskDelay(pdMS_TO_TICKS(poll_ms));
			elapsed += poll_ms;
		}
		if (!args.completed) {
			ESP_LOGW(TAG, "WASM execution exceeded %" PRIu32 " ms watchdog — "
					 "signalling cooperative cancellation", timeout_ms);

			s_cancelled = true;

			// Forcibly terminate the WASM instance so that
			// wasm_runtime_call_wasm() returns, even if the
			// WASM code is stuck in an infinite loop that
			// never calls a host function.
			if (args.module_inst != nullptr) {
				ESP_LOGW(TAG, "Calling wasm_runtime_terminate()");
				wasm_runtime_terminate(args.module_inst);
			}

			// Wait for the thread to finish — after terminate()
			// the interpreter will raise a trap and
			// wasm_runtime_call_wasm() returns (with false),
			// so the pthread should exit promptly.
			while (!args.completed) {
				vTaskDelay(pdMS_TO_TICKS(poll_ms));
			}

			pthread_join(thread, nullptr);
			s_cancelled = false;
			return SandboxResult::Timeout;
		}
	} else {
		// No timeout — just wait forever
		while (!args.completed) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	// Thread has finished — ensure it's joined to reclaim resources
	pthread_join(thread, nullptr);

	// Reset cancellation flag for next invocation
	s_cancelled = false;

	return args.result;
}

bool was_cancelled()
{
	return s_cancelled.load();
}

bool is_running()
{
	return s_running.load();
}

void destroy_sandbox()
{
	if (!s_initialized) {
		return;
	}

	wasm_runtime_destroy();
	s_initialized = false;

	ESP_LOGI(TAG, "WAMR sandbox destroyed");
}

}  // namespace wasm
