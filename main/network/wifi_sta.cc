#include "network/wifi_sta.h"

#include <cstring>
#include <string>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
#include "esp_eap_client.h"
#endif

static const char *TAG = "wifi_sta";

namespace network {
namespace {

/* ── NVS namespace for STA credentials ──────────────────────── */
constexpr const char *NVS_NS = "wifi_sta";
constexpr const char *NVS_KEY_SSID = "ssid";
constexpr const char *NVS_KEY_PASS = "password";

/* 802.1X. NVS keys are capped at 15 characters, hence the abbreviations. */
constexpr const char *NVS_KEY_EAP_METHOD = "eap_method";
constexpr const char *NVS_KEY_EAP_PHASE2 = "eap_phase2";
constexpr const char *NVS_KEY_EAP_IDENT  = "eap_ident";
constexpr const char *NVS_KEY_EAP_USER   = "eap_user";
constexpr const char *NVS_KEY_EAP_PASS   = "eap_pass";
constexpr const char *NVS_KEY_EAP_CA     = "eap_ca";
constexpr const char *NVS_KEY_EAP_CRT    = "eap_crt";
constexpr const char *NVS_KEY_EAP_KEY    = "eap_key";

/* ── Event bits ─────────────────────────────────────────────── */
EventGroupHandle_t s_evt_group = nullptr;
constexpr int BIT_CONNECTED   = BIT0;
constexpr int BIT_DISCONNECTED = BIT1;
constexpr int BIT_FAILED      = BIT2;

/* ── State ──────────────────────────────────────────────────── */
esp_netif_t *s_sta_netif = nullptr;
bool s_initialized = false;
StaState s_state = StaState::Disconnected;
std::string s_current_ssid;
std::string s_current_ip;

/* The live 802.1X credentials.
 *
 * This is deliberately a long-lived object rather than a parameter passed
 * down the stack. esp_eap_client_set_ca_cert() and
 * esp_eap_client_set_certificate_and_key() store the POINTER they are given
 * — they do not copy the PEM — so the buffers have to outlive the whole
 * association, not just the call. Identity/username/password are copied by
 * the supplicant, but keeping them here too means a reconnect after a
 * roaming drop does not need to go back to NVS.
 */
EapConfig s_eap;

/* 802.11 disconnect reasons, by number.
 *
 * The bare integer sends you to a table on the internet every time, and the
 * distinction it hides is the one that matters: 15 means you typed the
 * password wrong, 4 means the router accepted you and then let go. Switching
 * on the literals rather than the WIFI_REASON_* enum keeps this compiling
 * across IDF versions that add or rename members.
 *
 * On this robot 4 (ASSOC_EXPIRE) deserves special mention. The ESP32 has ONE
 * radio, and this firmware runs WIFI_MODE_APSTA — the MPX-Dog hotspot and the
 * join to your home network at the same time. They cannot sit on different
 * channels, so when the STA associates the driver drags the softAP over to
 * the router's channel, which is the "wifi:new:<1,1>, old:<6,1>" line right
 * above the failure. That switch lands in the middle of the association and
 * the association sometimes does not survive it. It is a race, not a fault:
 * the retry normally succeeds, which is why the robot ends up online anyway.
 *
 * On an 802.1X network the same numbers mean subtly different things, and the
 * enterprise-specific notes below are the ones worth reading first: 23 is the
 * RADIUS server saying no, and 15 stops meaning "wrong PSK" because there is
 * no PSK.
 */
static const char *wifi_reason_name(int reason)
{
    switch (reason) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE — router accepted then dropped us; on this "
                     "robot usually the AP+STA channel switch";
    case 5:   return "ASSOC_TOOMANY — the router is at its client limit";
    case 6:   return "NOT_AUTHED";
    case 7:   return "NOT_ASSOCED";
    case 8:   return "ASSOC_LEAVE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT — wrong password on a personal "
                     "network; on 802.1X, EAP finished but the key exchange "
                     "did not";
    case 23:  return "802_1X_AUTH_FAILED — the school's RADIUS server "
                     "rejected the account: wrong username or password, or "
                     "the identity needs the @domain suffix";
    case 200: return "BEACON_TIMEOUT — out of range, or the router went away";
    case 201: return "NO_AP_FOUND — wrong SSID, or it is 5 GHz only "
                     "(this radio is 2.4 GHz)";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "see esp_wifi_types.h";
    }
}

void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
        s_state = StaState::Connecting;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "STA disconnected (reason=%d: %s)",
                 event->reason, wifi_reason_name(event->reason));

        s_state = StaState::Disconnected;
        s_current_ip.clear();

        if (event->reason == WIFI_REASON_AUTH_EXPIRE ||
            event->reason == WIFI_REASON_AUTH_FAIL ||
            event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
            event->reason == WIFI_REASON_NO_AP_FOUND ||
            event->reason == WIFI_REASON_802_1X_AUTH_FAILED) {
            // Credential or availability issue — mark failed
            s_state = StaState::Failed;
            if (s_evt_group) xEventGroupSetBits(s_evt_group, BIT_FAILED);
        } else {
            // Transient — auto-reconnect will be attempted by IDF
            if (s_evt_group) xEventGroupSetBits(s_evt_group, BIT_DISCONNECTED);
        }
    }
}

void ip_event_handler(void *arg, esp_event_base_t base,
                      int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        s_current_ip = ip_str;
        s_state = StaState::Connected;

        ESP_LOGI(TAG, "STA got IP: %s", s_current_ip.c_str());
        if (s_evt_group) xEventGroupSetBits(s_evt_group, BIT_CONNECTED);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "STA lost IP");
        s_current_ip.clear();
        s_state = StaState::Disconnected;
    }
}

/* ── NVS helpers for variable-length strings ────────────────────
 *
 * A CA certificate or a client key is a couple of kilobytes of PEM, so the
 * old fixed char[64] is not an option for those. nvs_get_str() with a null
 * out-pointer reports the required size; ask, then read.
 */
static bool nvs_read_string(nvs_handle_t nvs, const char *key, std::string &out)
{
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) {
        out.clear();
        return false;
    }
    // len includes the NUL terminator.
    std::string buf(len, '\0');
    if (nvs_get_str(nvs, key, buf.data(), &len) != ESP_OK) {
        out.clear();
        return false;
    }
    buf.resize(len > 0 ? len - 1 : 0);
    out = std::move(buf);
    return true;
}

/* Write when non-empty, erase when empty, so a field the user cleared does
 * not survive as a stale value from a previous network. */
static esp_err_t nvs_write_string(nvs_handle_t nvs, const char *key,
                                  const std::string &value)
{
    if (value.empty()) {
        esp_err_t ret = nvs_erase_key(nvs, key);
        return (ret == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : ret;
    }
    return nvs_set_str(nvs, key, value.c_str());
}

static uint8_t nvs_read_u8(nvs_handle_t nvs, const char *key, uint8_t def)
{
    uint8_t v = def;
    if (nvs_get_u8(nvs, key, &v) != ESP_OK) return def;
    return v;
}

/* ── NVS persistence ────────────────────────────────────────── */
static bool save_credentials(const char *ssid, const char *password,
                             const EapConfig &eap)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s'", NVS_NS);
        return false;
    }

    esp_err_t ret;
    ret = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS write ssid failed: %s", esp_err_to_name(ret));
        nvs_close(nvs);
        return false;
    }

    ret = nvs_set_str(nvs, NVS_KEY_PASS, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS write password failed: %s", esp_err_to_name(ret));
        nvs_close(nvs);
        return false;
    }

    // 802.1X. Written unconditionally so that switching from the school
    // network back to a home network clears the enterprise fields instead of
    // leaving them to be picked up on the next boot.
    nvs_set_u8(nvs, NVS_KEY_EAP_METHOD, static_cast<uint8_t>(eap.method));
    nvs_set_u8(nvs, NVS_KEY_EAP_PHASE2, static_cast<uint8_t>(eap.phase2));
    nvs_write_string(nvs, NVS_KEY_EAP_IDENT, eap.identity);
    nvs_write_string(nvs, NVS_KEY_EAP_USER,  eap.username);
    nvs_write_string(nvs, NVS_KEY_EAP_PASS,  eap.password);
    nvs_write_string(nvs, NVS_KEY_EAP_CA,    eap.ca_cert);
    nvs_write_string(nvs, NVS_KEY_EAP_CRT,   eap.client_cert);
    nvs_write_string(nvs, NVS_KEY_EAP_KEY,   eap.client_key);

    ret = nvs_commit(nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(ret));
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved STA credentials to NVS (%s)",
             eap.method == EapMethod::None ? "personal" : "802.1X");
    return true;
}

static bool load_credentials(std::string &ssid, std::string &password,
                             EapConfig &eap)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    if (!nvs_read_string(nvs, NVS_KEY_SSID, ssid)) {
        nvs_close(nvs);
        return false;
    }

    nvs_read_string(nvs, NVS_KEY_PASS, password);

    uint8_t method = nvs_read_u8(nvs, NVS_KEY_EAP_METHOD,
                                 static_cast<uint8_t>(EapMethod::None));
    uint8_t phase2 = nvs_read_u8(nvs, NVS_KEY_EAP_PHASE2,
                                 static_cast<uint8_t>(EapPhase2::Mschapv2));
    if (method > static_cast<uint8_t>(EapMethod::Tls)) {
        method = static_cast<uint8_t>(EapMethod::None);
    }
    if (phase2 > static_cast<uint8_t>(EapPhase2::Chap)) {
        phase2 = static_cast<uint8_t>(EapPhase2::Mschapv2);
    }
    eap.method = static_cast<EapMethod>(method);
    eap.phase2 = static_cast<EapPhase2>(phase2);

    nvs_read_string(nvs, NVS_KEY_EAP_IDENT, eap.identity);
    nvs_read_string(nvs, NVS_KEY_EAP_USER,  eap.username);
    nvs_read_string(nvs, NVS_KEY_EAP_PASS,  eap.password);
    nvs_read_string(nvs, NVS_KEY_EAP_CA,    eap.ca_cert);
    nvs_read_string(nvs, NVS_KEY_EAP_CRT,   eap.client_cert);
    nvs_read_string(nvs, NVS_KEY_EAP_KEY,   eap.client_key);

    nvs_close(nvs);
    return true;
}

static void erase_credentials()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASS);
    nvs_erase_key(nvs, NVS_KEY_EAP_METHOD);
    nvs_erase_key(nvs, NVS_KEY_EAP_PHASE2);
    nvs_erase_key(nvs, NVS_KEY_EAP_IDENT);
    nvs_erase_key(nvs, NVS_KEY_EAP_USER);
    nvs_erase_key(nvs, NVS_KEY_EAP_PASS);
    nvs_erase_key(nvs, NVS_KEY_EAP_CA);
    nvs_erase_key(nvs, NVS_KEY_EAP_CRT);
    nvs_erase_key(nvs, NVS_KEY_EAP_KEY);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Erased STA credentials from NVS");
}

/* ── 802.1X supplicant setup ────────────────────────────────────
 *
 * Everything here reads from s_eap rather than from a parameter: the IDF
 * keeps the certificate pointers it is handed, so they must refer to storage
 * that outlives this function. See the note on s_eap above.
 *
 * Returns false if enterprise support is not compiled in, which is the one
 * failure worth reporting up rather than logging and limping on.
 */
static bool apply_enterprise_config()
{
#if !CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
    ESP_LOGE(TAG, "802.1X requested but CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT is "
                  "off — enable 'Wi-Fi Enterprise support' under Component "
                  "config > Wi-Fi and rebuild");
    return false;
#else
    // Start from a clean slate. Reconnecting to a different network without
    // this leaves the previous account's username in place, and the failure
    // that produces (RADIUS reject for a user you are not trying to be) is
    // deeply confusing to read in the log.
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();

    // Nothing below uses ESP_ERROR_CHECK. Every one of these calls can fail
    // on input the user typed — a username over 128 bytes, a PEM that is not
    // a PEM — and an abort() there would put the robot in a boot loop it
    // cannot be talked out of, because the bad credentials are in NVS and get
    // replayed on the next boot. Failing the connect and leaving the softAP
    // up keeps the PWA reachable so the user can fix the typo.
    auto step = [](const char *what, esp_err_t err) {
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "802.1X %s failed: %s", what, esp_err_to_name(err));
            return false;
        }
        return true;
    };

    // The outer identity travels in the clear. If the user did not give one,
    // use the username — what a phone does when you leave "anonymous
    // identity" blank.
    const std::string &outer =
        s_eap.identity.empty() ? s_eap.username : s_eap.identity;
    if (!outer.empty()) {
        if (!step("set_identity", esp_eap_client_set_identity(
                reinterpret_cast<const unsigned char *>(outer.c_str()),
                static_cast<int>(outer.size())))) {
            return false;
        }
    }

    if (!s_eap.ca_cert.empty()) {
        if (!step("set_ca_cert", esp_eap_client_set_ca_cert(
                reinterpret_cast<const unsigned char *>(s_eap.ca_cert.c_str()),
                static_cast<int>(s_eap.ca_cert.size() + 1)))) {
            return false;
        }
    } else {
        ESP_LOGW(TAG, "No CA certificate — the RADIUS server's identity will "
                      "not be verified");
    }

    // The ESP32 boots with no wall clock, so every certificate looks like it
    // was issued in 1970 and validity-period checks fail even when the chain
    // is perfectly good. Disabling the time check is what the IDF's own
    // wifi_enterprise example does, for the same reason.
    esp_eap_client_set_disable_time_check(true);

    if (s_eap.method == EapMethod::Tls) {
        if (s_eap.client_cert.empty() || s_eap.client_key.empty()) {
            ESP_LOGE(TAG, "EAP-TLS needs both a client certificate and a key");
            return false;
        }
        if (!step("set_certificate_and_key", esp_eap_client_set_certificate_and_key(
                reinterpret_cast<const unsigned char *>(s_eap.client_cert.c_str()),
                static_cast<int>(s_eap.client_cert.size() + 1),
                reinterpret_cast<const unsigned char *>(s_eap.client_key.c_str()),
                static_cast<int>(s_eap.client_key.size() + 1),
                nullptr, 0))) {
            return false;
        }
    } else {
        // PEAP and TTLS both authenticate inside the tunnel with a plain
        // username and password. PEAP's phase 2 is MSCHAPv2 and not
        // selectable; TTLS's is.
        if (s_eap.method == EapMethod::Ttls) {
            esp_eap_ttls_phase2_types phase2 = ESP_EAP_TTLS_PHASE2_MSCHAPV2;
            switch (s_eap.phase2) {
            case EapPhase2::Mschapv2: phase2 = ESP_EAP_TTLS_PHASE2_MSCHAPV2; break;
            case EapPhase2::Mschap:   phase2 = ESP_EAP_TTLS_PHASE2_MSCHAP;   break;
            case EapPhase2::Pap:      phase2 = ESP_EAP_TTLS_PHASE2_PAP;      break;
            case EapPhase2::Chap:     phase2 = ESP_EAP_TTLS_PHASE2_CHAP;     break;
            }
            if (!step("set_ttls_phase2_method",
                      esp_eap_client_set_ttls_phase2_method(phase2))) {
                return false;
            }
        }

        if (s_eap.username.empty()) {
            ESP_LOGE(TAG, "PEAP/TTLS needs a username");
            return false;
        }
        if (!step("set_username", esp_eap_client_set_username(
                reinterpret_cast<const unsigned char *>(s_eap.username.c_str()),
                static_cast<int>(s_eap.username.size())))) {
            return false;
        }
        if (!step("set_password", esp_eap_client_set_password(
                reinterpret_cast<const unsigned char *>(s_eap.password.c_str()),
                static_cast<int>(s_eap.password.size())))) {
            return false;
        }
    }

    if (!step("enterprise_enable", esp_wifi_sta_enterprise_enable())) {
        return false;
    }
    return true;
#endif
}

/* Turn the supplicant back off when joining a personal network.
 *
 * Without this, a robot that was on the school network and is then pointed
 * at a home router keeps trying to run EAP against an AP that has never
 * heard of 802.1X, and the association simply never completes.
 */
static void clear_enterprise_config()
{
#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();
    esp_wifi_sta_enterprise_disable();
#endif
}

/* ── The one connect path ───────────────────────────────────────
 *
 * Personal and enterprise differ only in what happens between
 * esp_wifi_set_config() and esp_wifi_start(), so they share everything else
 * rather than living as two near-identical copies that drift apart.
 */
static bool connect_internal(const char *ssid, const char *password,
                             const EapConfig &eap)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "STA not initialised");
        return false;
    }

    if (!ssid || std::strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        return false;
    }

    if (std::strlen(ssid) > 31) {
        ESP_LOGE(TAG, "SSID too long (%zu chars, max 31)", std::strlen(ssid));
        return false;
    }

    const bool enterprise = (eap.method != EapMethod::None);
    const char *pass = password ? password : "";

    if (!enterprise && std::strlen(pass) > 63) {
        ESP_LOGE(TAG, "Password too long (%zu chars, max 63)", std::strlen(pass));
        return false;
    }

    // Stop Wi-Fi so we can reconfigure (already started by AP init)
    esp_wifi_stop();

    // Set Wi-Fi mode to AP+STA (dual mode)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure STA
    wifi_config_t sta_config = {};
    std::strncpy(reinterpret_cast<char *>(sta_config.sta.ssid),
                 ssid, sizeof(sta_config.sta.ssid) - 1);
    // An 802.1X network has no pre-shared key: the field stays empty and the
    // credentials go to the supplicant instead.
    if (!enterprise && std::strlen(pass) > 0) {
        std::strncpy(reinterpret_cast<char *>(sta_config.sta.password),
                     pass, sizeof(sta_config.sta.password) - 1);
    }
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.rssi = -127;
    // WPA2-Enterprise advertises WIFI_AUTH_WPA2_ENTERPRISE. Leaving the
    // threshold at the default would have the driver skip those APs as
    // "not secure enough to match", so it is lowered to OPEN and the
    // supplicant decides.
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // WPA3-Enterprise APs require management frame protection. Advertising
    // the capability costs nothing on a WPA2 network and is the difference
    // between joining and not on a WPA3 one.
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    s_current_ssid = ssid;
    s_state = StaState::Connecting;

    // Persist to NVS
    save_credentials(ssid, pass, eap);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    // The supplicant is configured after esp_wifi_set_config() and before
    // esp_wifi_start(), the order the IDF's wifi_enterprise example uses.
    if (enterprise) {
        s_eap = eap;
        if (!apply_enterprise_config()) {
            s_state = StaState::Failed;
            // Bring the radio back up anyway so the softAP — and with it the
            // PWA the user is reading this failure on — does not stay down.
            esp_wifi_start();
            return false;
        }
    } else {
        s_eap = EapConfig{};
        clear_enterprise_config();
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        s_state = StaState::Failed;
        return false;
    }

    if (enterprise) {
        const char *method_name =
            eap.method == EapMethod::Peap ? "PEAP" :
            eap.method == EapMethod::Ttls ? "TTLS" : "TLS";
        ESP_LOGI(TAG, "Connecting to 802.1X network '%s' (%s, identity '%s')",
                 ssid, method_name,
                 (eap.identity.empty() ? eap.username : eap.identity).c_str());
    } else {
        ESP_LOGI(TAG, "Connecting to STA network '%s'", ssid);
    }
    return true;
}

}  // anonymous namespace

/* ── Public API ─────────────────────────────────────────────── */

bool init_wifi_sta()
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Wi-Fi STA already initialised");
        return true;
    }

    // Create event group
    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    // Register event handlers for STA events
    esp_event_handler_instance_t wifi_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, &wifi_handler));

    esp_event_handler_instance_t ip_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, nullptr, &ip_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, nullptr, &ip_handler));

    // Create the STA netif
    s_sta_netif = esp_netif_create_default_wifi_sta();
    assert(s_sta_netif);

    s_initialized = true;

    // Try to load saved credentials and auto-connect
    std::string saved_ssid, saved_password;
    EapConfig saved_eap;
    if (load_credentials(saved_ssid, saved_password, saved_eap) &&
        !saved_ssid.empty()) {
        ESP_LOGI(TAG, "Found saved %s credentials for '%s' — auto-connecting",
                 saved_eap.method == EapMethod::None ? "STA" : "802.1X",
                 saved_ssid.c_str());
        connect_internal(saved_ssid.c_str(), saved_password.c_str(), saved_eap);
    } else {
        ESP_LOGI(TAG, "No saved STA credentials — staying in AP-only mode");
    }

    return true;
}

void deinit_wifi_sta()
{
    if (!s_initialized) return;

    wifi_sta_disconnect();

    if (s_evt_group) {
        vEventGroupDelete(s_evt_group);
        s_evt_group = nullptr;
    }

    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = nullptr;
    s_initialized = false;
    s_state = StaState::Disconnected;
    s_current_ssid.clear();
    s_current_ip.clear();
    s_eap = EapConfig{};

    ESP_LOGI(TAG, "Wi-Fi STA deinitialised");
}

bool wifi_sta_connect(const char *ssid, const char *password)
{
    return connect_internal(ssid, password, EapConfig{});
}

bool wifi_sta_connect_eap(const char *ssid, const EapConfig &eap)
{
    if (eap.method == EapMethod::None) {
        ESP_LOGE(TAG, "wifi_sta_connect_eap called with method None — use "
                      "wifi_sta_connect for personal networks");
        return false;
    }
    return connect_internal(ssid, "", eap);
}

void wifi_sta_disconnect()
{
    if (s_state != StaState::Disconnected) {
        esp_wifi_disconnect();
        s_state = StaState::Disconnected;
        s_current_ip.clear();
        ESP_LOGI(TAG, "STA disconnected");
    }
}

void wifi_sta_forget()
{
    wifi_sta_disconnect();
    erase_credentials();
    clear_enterprise_config();
    s_current_ssid.clear();
    s_eap = EapConfig{};
}

StaState wifi_sta_get_state()
{
    return s_state;
}

std::string wifi_sta_get_ip()
{
    return s_current_ip;
}

std::string wifi_sta_get_ssid()
{
    return s_current_ssid;
}

EapMethod wifi_sta_get_eap_method()
{
    return s_eap.method;
}

std::string wifi_sta_get_eap_identity()
{
    if (s_eap.method == EapMethod::None) return {};
    return s_eap.identity.empty() ? s_eap.username : s_eap.identity;
}

}  // namespace network
