#include <cstddef>
#include <cstdint>

#include "esp_log.h"
#include "nvs_flash.h"

#include "fs/littlefs_manager.h"
#include "lua/lua_vm.h"
#include "skills/autorun.h"
#include "skills/events.h"
#include "skills/registry.h"
#include "network/chat_ws.h"
#include "network/http_server.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"
#include "network/www_assets.h"
#include "robot/robot.h"
#include "util/log_ring.h"
#include "wasm/wasm_sandbox.h"

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
	// First, so the ring captures boot as well — the failures that are
	// hardest to debug over Wi-Fi are the ones that happen before the web
	// server is even up. Chains to the UART sink, so serial is unchanged.
	util::log_ring_init();

	init_nvs();

	// ── Robot HAL (servo bus, gait task on Core 1) ─────────────
	if (!robot::init()) {
		ESP_LOGE(TAG, "Robot HAL init failed — continuing without servo control");
	}

	// Mount LittleFS (for .wasm skills and saved Lua scripts — the PWA is
	// no longer stored here, so a full partition can't take the web UI down)
	if (!fs::init_littlefs()) {
		ESP_LOGE(TAG, "LittleFS mount failed — wasm sandbox unavailable");
		return;
	}

	{
		std::size_t total = 0, used = 0;
		if (fs::stats(total, used) && total > 0 && used * 10 >= total * 9) {
			ESP_LOGW(TAG, "Storage partition is %zu%% full (%zu of %zu KiB) — "
			              "skill uploads and script saves will fail",
			         (used * 100) / total, used / 1024, total / 1024);
			ESP_LOGW(TAG, "To wipe it: esptool.py --chip esp32s3 -p <port> "
			              "erase_region 0x290000 0xd70000");
		}
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

	// ── PWA assets ────────────────────────────────────────────
	// Nothing to deploy: assets are served straight from the memory-mapped
	// firmware image, so there is no copy on LittleFS to keep in sync and no
	// way for a full filesystem to take the web UI down.
	ESP_LOGI(TAG, "PWA: %d assets, %zu KiB gzipped, served from firmware",
	         network::embedded_asset_count(),
	         network::embedded_asset_bytes() / 1024);

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

	// ── Skill registry, triggers and autorun ──────────────────
	//
	// Ordering matters and is not arbitrary: the registry reads the .wasm
	// files, so it needs LittleFS; autorun_boot() starts a skill, so it needs
	// the sandbox and the robot HAL; and both come AFTER the HTTP server, so
	// that if an autorun skill misbehaves the web UI is already up and the
	// user can uninstall it. That last one is the difference between a bad
	// skill and a brick.
	skills::rescan();
	skills::events_start();
	skills::autorun_boot();

	// ── Lua scripting VM ──────────────────────────────────────
	if (lua_init() != ESP_OK) {
		ESP_LOGE(TAG, "Lua VM init failed — continuing without Lua scripting");
	} else {
		ESP_LOGI(TAG, "Lua VM ready — robot bindings registered");
	}
}