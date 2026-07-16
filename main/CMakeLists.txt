idf_component_register(
    SRCS
        "main.cc"
        "fs/littlefs_manager.cc"
        "wasm/wasm_sandbox.cc"
        "sdk/wasm_host_functions.cc"
        "network/wifi_ap.cc"
        "network/wifi_sta.cc"
        "network/http_server.cc"
        "network/www_assets.cc"
        "network/crypto.cc"
        "network/chat_ws.cc"
        "robot/robot.cc"
        "robot/stanford_gait.cc"
        "robot/stanford_kinematics.cc"
        "robot/imu.cc"
        "lua/lua_vm.cc"
        "lua/lua_bindings.cc"
    INCLUDE_DIRS "." "fs" "wasm" "sdk" "network" "robot" "lua"
    EMBED_FILES
        # PWA static assets (www output from pwa-redesign/ build)
        "../pwa-redesign/www/icon.svg"
        "../pwa-redesign/www/icon.svg.gz"
        "../pwa-redesign/www/index.html"
        "../pwa-redesign/www/index.html.gz"
        "../pwa-redesign/www/index.css"
        "../pwa-redesign/www/index.css.gz"
        "../pwa-redesign/www/m.js"
        "../pwa-redesign/www/m.js.gz"
        "../pwa-redesign/www/manifest.json"
        "../pwa-redesign/www/manifest.json.gz"
        "../pwa-redesign/www/sw.js"
        "../pwa-redesign/www/sw.js.gz"
        "../pwa-redesign/www/md.svg"
        "../pwa-redesign/www/md.svg.gz"
        "../pwa-redesign/www/eye-open.svg"
        "../pwa-redesign/www/eye-open.svg.gz"
        "../pwa-redesign/www/eye-close.svg"
        "../pwa-redesign/www/eye-close.svg.gz"
    REQUIRES
        nvs_flash
        wasm-micro-runtime
        lua
        littlefs
        driver
        ftservo
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