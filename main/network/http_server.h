#pragma once

#include <cstddef>
#include <cstdint>

namespace network {

/**
 * @brief Start the HTTP / WebSocket server on port 80.
 *
 * Serves PWA static assets directly from the memory-mapped firmware image
 * (see network/www_assets.h). Assets are stored pre-gzipped only and are
 * always sent with Content-Encoding: gzip; nothing is read from LittleFS,
 * so the UI is unaffected by the state of the storage partition.
 *
 * Provides WebSocket telemetry at /v1/telemetry/stream.
 *
 * Must be called after init_wifi_ap().
 *
 * @return true on success.
 */
bool start_http_server();

/**
 * @brief Stop the HTTP server.
 */
void stop_http_server();

}  // namespace network
