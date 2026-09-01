#pragma once

#include <cstdint>
#include <string>

namespace network {

/**
 * @brief EAP method for WPA2/WPA3-Enterprise (802.1X) networks.
 *
 * `None` means an ordinary personal network (open or WPA2-PSK) — the path
 * this file has always taken. Anything else routes through the ESP-IDF
 * `esp_eap_client` API, which is what campus and corporate networks want:
 * they hand out a username and a password, not a shared pre-shared key.
 */
enum class EapMethod : uint8_t {
    None = 0,   ///< Personal: open or WPA/WPA2/WPA3-PSK
    Peap,       ///< PEAPv0 + MSCHAPv2 — by far the most common on campuses
    Ttls,       ///< EAP-TTLS with a selectable phase-2 method
    Tls,        ///< EAP-TLS — client certificate instead of a password
};

/**
 * @brief Inner ("phase 2") authentication for EAP-TTLS.
 *
 * Ignored for PEAP (always MSCHAPv2 here) and for TLS (no phase 2).
 */
enum class EapPhase2 : uint8_t {
    Mschapv2 = 0,
    Mschap,
    Pap,
    Chap,
};

/**
 * @brief Everything an 802.1X association needs.
 *
 * Empty strings mean "not supplied", and the connect path treats that as
 * "leave it at the supplicant's default" rather than pushing an empty value.
 *
 * `identity` is the OUTER identity — the one sent in the clear before the
 * TLS tunnel comes up. Schools that run anonymous outer identities expect
 * something like "anonymous@uni.edu" here while `username` holds the real
 * account. If it is left empty the username is used for both, which is what
 * every phone does by default and what most networks accept.
 *
 * `ca_cert` is optional. With it, the robot verifies the RADIUS server's
 * certificate; without it, it does not, exactly like tapping through the
 * "trust this certificate" prompt on a laptop.
 */
struct EapConfig {
    EapMethod  method   = EapMethod::None;
    EapPhase2  phase2   = EapPhase2::Mschapv2;
    std::string identity;     ///< outer / anonymous identity (optional)
    std::string username;     ///< inner username (PEAP / TTLS)
    std::string password;     ///< inner password (PEAP / TTLS)
    std::string ca_cert;      ///< PEM root CA (optional; empty = no validation)
    std::string client_cert;  ///< PEM client certificate (EAP-TLS only)
    std::string client_key;   ///< PEM client private key (EAP-TLS only)
};

/**
 * @brief Wi-Fi STA mode configuration.
 *
 * SSID and password are persisted to NVS under the "wifi_sta" namespace
 * so the robot auto-connects on subsequent boots.
 */
struct StaConfig {
    std::string ssid;
    std::string password;
    EapConfig   eap;
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
 * @brief Attempt to connect to a personal (open or WPA2-PSK) network.
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
 * @brief Attempt to connect to a WPA2/WPA3-Enterprise (802.1X) network.
 *
 * Same contract as wifi_sta_connect(), but the credentials in @p eap are used
 * instead of a pre-shared key. Everything is persisted to NVS, including the
 * certificates, so the robot re-joins the school network on its own after a
 * reboot.
 *
 * @param ssid The network SSID (max 31 chars).
 * @param eap  The 802.1X credentials. `eap.method` must not be None.
 * @return true if the connection attempt was started.
 */
bool wifi_sta_connect_eap(const char *ssid, const EapConfig &eap);

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
 * @brief The EAP method of the current (or last-attempted) network.
 *
 * EapMethod::None for a personal network. Lets /v1/wifi/status tell the PWA
 * which form to re-open without handing the credentials back out.
 */
EapMethod wifi_sta_get_eap_method();

/**
 * @brief The identity the current 802.1X association is using.
 *
 * The outer identity if one was given, otherwise the username. Empty on a
 * personal network. Safe to display — it is sent in the clear on the air.
 */
std::string wifi_sta_get_eap_identity();

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
