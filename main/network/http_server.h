#pragma once

#include <cstddef>
#include <cstdint>

namespace network {

/**
 * @brief Root directory inside LittleFS where www assets are stored.
 */
constexpr const char *WWW_ROOT = "/fs/www";

/**
 * @brief Start the HTTP / WebSocket server on port 80.
 *
 * Serves PWA static assets from LittleFS under WWW_ROOT.
 * Supports pre-compressed .gz files with Content-Encoding: gzip.
 * Provides WebSocket telemetry at /v1/telemetry/stream.
 *
 * Must be called after init_wifi_ap() and fs::init_littlefs().
 *
 * @return true on success.
 */
bool start_http_server();

/**
 * @brief Stop the HTTP server.
 */
void stop_http_server();

}  // namespace network
