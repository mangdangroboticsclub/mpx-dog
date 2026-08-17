# ── Source files (common to all designs) ─────────────────────
set(sources
    "main.cc"
    "fs/littlefs_manager.cc"
    "wasm/wasm_sandbox.cc"
    "wasm/wasm_decrypt.cc"
    "sdk/wasm_host_functions.cc"
    "network/wifi_ap.cc"
    "network/wifi_sta.cc"
    "network/http_server.cc"
    "network/www_assets.cc"
    "network/crypto.cc"
    "network/chat_ws.cc"
    "network/marketplace_proxy.cc"
    "robot/robot.cc"
    "robot/driver_board.c"
    "robot/stanford_gait.cc"
    "robot/stanford_kinematics.cc"
    "robot/imu.cc"
    "lua/lua_vm.cc"
    "lua/lua_bindings.cc"
    "util/log_ring.cc"
)

# ── PWA assets ───────────────────────────────────────────────
#
# ONLY the pre-gzipped variants are embedded. The uncompressed copies cost
# ~389 KB of the 2.5 MB app partition and were never served: the HTTP server
# always responds with Content-Encoding: gzip (see network/www_assets.cc).
# They are also no longer copied to LittleFS — assets are served directly from
# the memory-mapped app image.
if(CONFIG_PWA_DESIGN_REDESIGN)
    message(STATUS "PWA design: redesign (light theme) — gzip-only assets")
    set(pwa_www_dir "../pwa-redesign/www")
    set(design_assets
        "../pwa-redesign/www/md.svg.gz"
        "../pwa-redesign/www/eye-open.svg.gz"
        "../pwa-redesign/www/eye-close.svg.gz"
    )
else()
    message(STATUS "PWA design: original (dark theme) — gzip-only assets")
    set(pwa_www_dir "../pwa/www")
    set(design_assets
        "../pwa/www/icon.svg.gz"
    )
endif()

# ── Assets shared by both designs (same names, different content) ──
set(common_assets
    "${pwa_www_dir}/index.html.gz"
    "${pwa_www_dir}/index.css.gz"
    "${pwa_www_dir}/m.js.gz"
    "${pwa_www_dir}/manifest.json.gz"
    "${pwa_www_dir}/sw.js.gz"
)

# ── MangDang Servo Studio ────────────────────────────────────
# Standalone servo tuning page served at /studio, independent of the Svelte
# app so it still loads when the PWA build is stale. Regenerate the .gz with
# tools/gzip-studio.mjs after editing studio.html.
set(studio_assets
    "../studio/studio.html.gz"
)

# ── Register component ───────────────────────────────────────
idf_component_register(
    SRCS        ${sources}
    INCLUDE_DIRS "." "fs" "wasm" "sdk" "network" "robot" "lua"
    EMBED_FILES
        ${common_assets}
        ${design_assets}
        ${studio_assets}
    REQUIRES
        nvs_flash
        wasm-micro-runtime
        lua
        littlefs
        driver
        esp_driver_spi
        esp_driver_gpio
        esp_http_server
        esp_wifi
        esp_netif
        lwip
        esp_event
        mbedtls
        json
)

# Lua requires 32-bit mode on Xtensa/RISC-V architectures.
# This must match the Lua component's own definition so that
# including lua.h from our code sees LUA_32BITS=1.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)