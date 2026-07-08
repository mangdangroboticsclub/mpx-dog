#include <cstdint>

#include "esp_log.h"
#include "nvs_flash.h"

#include "fs/littlefs_manager.h"
#include "lua/lua_vm.h"
#include "network/chat_ws.h"
#include "network/http_server.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"
#include "network/www_assets.h"
#include "robot/robot.h"
#include "wasm/wasm_sandbox.h"

// Embedded .wasm binary — linked via EMBED_FILES in CMakeLists.txt
extern const uint8_t test_skill_wasm_start[] asm("_binary_test_skill_wasm_start");
extern const uint8_t test_skill_wasm_end[]   asm("_binary_test_skill_wasm_end");

static const char *TAG = "main";

static void init_nvs()
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
}

extern "C" void app_main(void)
{
	init_nvs();

	// ── Robot HAL (servo bus, gait task on Core 1) ─────────────
	if (!robot::init()) {
		ESP_LOGE(TAG, "Robot HAL init failed — continuing without servo control");
	}

	// Mount LittleFS (for .wasm storage and PWA assets)
	if (!fs::init_littlefs()) {
		ESP_LOGE(TAG, "LittleFS mount failed — wasm sandbox unavailable");
		return;
	}

	// ── Network: start Wi-Fi AP (Core 0, Priority 6) ──────────
	if (!network::init_wifi_ap()) {
		ESP_LOGE(TAG, "Wi-Fi AP init failed");
		return;
	}

	// ── Network: initialise Wi-Fi STA (auto-connects if saved) ─
	if (!network::init_wifi_sta()) {
		ESP_LOGW(TAG, "Wi-Fi STA init failed — continuing in AP-only mode");
	}

	// ── Deploy embedded PWA assets to LittleFS ────────────────
	if (!network::deploy_www_assets()) {
		ESP_LOGE(TAG, "PWA asset deployment failed");
		return;
	}

	// ── Start HTTP + WebSocket server (Core 0, Priority 6) ───
	if (!network::start_http_server()) {
		ESP_LOGE(TAG, "HTTP server init failed");
		return;
	}

	// ── Chat pipeline (echo / encrypted cloud proxy) ─────────
	if (!network::init_chat_pipeline()) {
		ESP_LOGW(TAG, "Chat pipeline init failed — chat UI unavailable");
	}

	// ── Wasm sandbox ──────────────────────────────────────────
	if (!wasm::init_sandbox()) {
		ESP_LOGE(TAG, "WAMR sandbox init failed");
		return;
	}

	// ── Lua scripting VM ──────────────────────────────────────
	if (lua_init() != ESP_OK) {
		ESP_LOGE(TAG, "Lua VM init failed — continuing without Lua scripting");
	} else {
		ESP_LOGI(TAG, "Lua VM ready — robot bindings registered");

		// ── Quick Lua smoke test ──────────────────────────────
		char lua_out[256];
		esp_err_t lua_ret = lua_run_string(
			R"(
				print("Hello from Lua on ESP32-S3!")
				local cfg = robot.get_config()
				print("Current config: period=" .. cfg.period ..
				      " height=" .. cfg.height ..
				      " stride=" .. cfg.stride)
			)",
			lua_out, sizeof(lua_out), 5000);

		if (lua_ret == ESP_OK) {
			ESP_LOGI(TAG, "✅ Lua smoke test passed:\n%s", lua_out);
		} else {
			ESP_LOGW(TAG, "Lua smoke test failed (err=%d): %s",
					 lua_ret, lua_out);
		}
	}

	// Run the embedded test WASM binary
	const size_t wasm_size = test_skill_wasm_end - test_skill_wasm_start;
	ESP_LOGI(TAG, "Running embedded WASM (%zu bytes)", wasm_size);

	wasm::SandboxResult result = wasm::load_and_run_bytes(
		test_skill_wasm_start, wasm_size, "on_start", 3000);

	if (result == wasm::SandboxResult::Success) {
		ESP_LOGI(TAG, "✅ Embedded WASM executed successfully");
	} else {
		ESP_LOGE(TAG, "❌ Embedded WASM execution failed (code=%d)",
				 static_cast<int>(result));
	}

}