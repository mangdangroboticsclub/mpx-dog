#pragma once

#include <cstdint>
#include <string>

namespace network {

/**
 * @brief Wi-Fi STA mode configuration.
 *
 * SSID and password are persisted to NVS under the "wifi_sta" namespace
 * so the robot auto-connects on subsequent boots.
 */
struct StaConfig {
    std::string ssid;
    std::string password;
};

/**
 * @brief Current STA connection state.
 */
enum class StaState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Failed,
};

/**
 * @brief Attempt to connect to a Wi-Fi network as a station.
 *
 * Credentials are persisted to NVS so they survive reboots.
 * Calling this while already connected will disconnect and reconnect.
 *
 * @param ssid     The network SSID (max 31 chars).
 * @param password The network password (max 63 chars, empty for open).
 * @return true if the connection attempt was started.
 */
bool wifi_sta_connect(const char *ssid, const char *password);

/**
 * @brief Disconnect from the current Wi-Fi network.
 *
 * Credentials are NOT erased from NVS — use wifi_sta_forget() for that.
 */
void wifi_sta_disconnect();

/**
 * @brief Forget saved credentials and disconnect.
 */
void wifi_sta_forget();

/**
 * @brief Get the current STA connection state.
 */
StaState wifi_sta_get_state();

/**
 * @brief Get the IP address assigned to the STA interface.
 *
 * @return IP string (e.g. "192.168.1.42") or empty string if not connected.
 */
std::string wifi_sta_get_ip();

/**
 * @brief Get the SSID of the currently connected (or last-attempted) network.
 */
std::string wifi_sta_get_ssid();

/**
 * @brief Initialise the STA interface.
 *
 * Must be called after nvs_flash_init() but before the HTTP server starts.
 * If saved credentials exist in NVS, an auto-connect is triggered.
 *
 * @return true on success.
 */
bool init_wifi_sta();

/**
 * @brief De-initialise the STA interface.
 */
void deinit_wifi_sta();

}  // namespace network
