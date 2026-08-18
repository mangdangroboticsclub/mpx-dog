#pragma once
#include <cstdint>
#include <cstddef>
typedef struct WASMExecEnv*      wasm_exec_env_t;
typedef struct WASMModuleInst*   wasm_module_inst_t;
typedef struct WASMModule*       wasm_module_t;
typedef struct WASMFunctionInst* wasm_function_inst_t;
typedef struct NativeSymbol { const char *symbol; void *func_ptr; const char *signature; void *attachment; } NativeSymbol;
typedef struct RuntimeInitArgs {
  int mem_alloc_type;
  struct { struct { void *malloc_func, *realloc_func, *free_func; } allocator; } mem_alloc_option;
} RuntimeInitArgs;
#define Alloc_With_Allocator 1
extern "C" {
bool wasm_runtime_full_init(RuntimeInitArgs*);
wasm_module_t wasm_runtime_load(uint8_t*, uint32_t, char*, uint32_t);
void wasm_runtime_unload(wasm_module_t);
wasm_module_inst_t wasm_runtime_instantiate(wasm_module_t, uint32_t, uint32_t, char*, uint32_t);
void wasm_runtime_deinstantiate(wasm_module_inst_t);
wasm_function_inst_t wasm_runtime_lookup_function(wasm_module_inst_t, const char*, const char*);
wasm_exec_env_t wasm_runtime_create_exec_env(wasm_module_inst_t, uint32_t);
void wasm_runtime_destroy_exec_env(wasm_exec_env_t);
bool wasm_runtime_call_wasm(wasm_exec_env_t, wasm_function_inst_t, uint32_t, uint32_t*);
const char *wasm_runtime_get_exception(wasm_module_inst_t);
void wasm_runtime_clear_exception(wasm_module_inst_t);
void wasm_runtime_terminate(wasm_module_inst_t);
bool wasm_runtime_register_natives(const char*, NativeSymbol*, uint32_t);
void wasm_runtime_destroy(void);
wasm_module_inst_t wasm_runtime_get_module_inst(wasm_exec_env_t);
void *wasm_runtime_addr_app_to_native(wasm_module_inst_t, uint32_t);
bool wasm_runtime_validate_app_addr(wasm_module_inst_t, uint32_t, uint32_t);
}
