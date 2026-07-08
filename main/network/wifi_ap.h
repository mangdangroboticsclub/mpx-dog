#pragma once

#include <cstdint>

namespace network {

/**
 * @brief Wi-Fi AP configuration defaults.
 */
constexpr const char *WIFI_AP_SSID       = "MPX-Dog";
constexpr const char *WIFI_AP_PASSWORD   = "";
constexpr uint8_t     WIFI_AP_MAX_CONN   = 4;
constexpr uint8_t     WIFI_AP_CHANNEL    = 6;

/**
 * @brief Static IP for the AP interface.
 */
constexpr const char *AP_IP_ADDR  = "192.168.2.1";
constexpr const char *AP_NETMASK  = "255.255.255.0";
constexpr const char *AP_GATEWAY  = "192.168.2.1";

/**
 * @brief Initialize Wi-Fi in Soft-AP mode with a static IP.
 *
 * Sets up the ESP32 as an access point at 192.168.2.1.
 * Must be called after nvs_flash_init() and before the HTTP server starts.
 *
 * @return true on success.
 */
bool init_wifi_ap();

/**
 * @brief Stop Wi-Fi AP and clean up.
 */
void deinit_wifi_ap();

}  // namespace network
