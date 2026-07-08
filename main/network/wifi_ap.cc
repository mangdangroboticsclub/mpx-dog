#include "network/wifi_ap.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi_ap";

static esp_netif_t *s_ap_netif = nullptr;
static bool s_initialized = false;

/* ── Helpers in the network namespace ──────────────────────── */
namespace network {
namespace {

void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        auto *event = static_cast<wifi_event_ap_staconnected_t *>(data);
        ESP_LOGI(TAG, "Station connected: " MACSTR " (aid=%d)",
                 MAC2STR(event->mac), event->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(data);
        ESP_LOGI(TAG, "Station disconnected: " MACSTR " (aid=%d)",
                 MAC2STR(event->mac), event->aid);
    }
}

bool set_static_ip()
{
    esp_netif_dns_info_t dns;
    esp_netif_ip_info_t ip{};

    ip.ip.addr       = esp_ip4addr_aton(AP_IP_ADDR);
    ip.netmask.addr  = esp_ip4addr_aton(AP_NETMASK);
    ip.gw.addr       = esp_ip4addr_aton(AP_GATEWAY);

    if (esp_netif_dhcps_stop(s_ap_netif) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop DHCP server");
        return false;
    }

    if (esp_netif_set_ip_info(s_ap_netif, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set static IP");
        return false;
    }

    // Set DNS to the AP IP itself
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    // Restart DHCP server with the new range
    esp_netif_dhcps_start(s_ap_netif);

    ESP_LOGI(TAG, "Static IP set to %s", AP_IP_ADDR);
    return true;
}

}  // anonymous namespace

/* ── Public API ─────────────────────────────────────────────── */

bool init_wifi_ap()
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Wi-Fi AP already initialised");
        return true;
    }

    // Initialise netif layer (idempotent)
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop if not already created
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, nullptr));

    // Create the AP netif
    s_ap_netif = esp_netif_create_default_wifi_ap();
    assert(s_ap_netif);

    // Init Wi-Fi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Set Wi-Fi to AP+STA dual mode (STA netif created by init_wifi_sta)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure AP
    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char *>(ap_config.ap.ssid),
                 WIFI_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(WIFI_AP_SSID));
    ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_config.ap.channel = WIFI_AP_CHANNEL;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;       // open network
    ap_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // Set static IP before starting
    if (!set_static_ip()) {
        ESP_LOGE(TAG, "Failed to configure static IP");
        return false;
    }

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started — SSID: \"%s\", IP: %s",
             WIFI_AP_SSID, AP_IP_ADDR);

    s_initialized = true;
    return true;
}

void deinit_wifi_ap()
{
    if (!s_initialized) return;

    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = nullptr;
    s_initialized = false;

    ESP_LOGI(TAG, "Wi-Fi AP deinitialised");
}

}  // namespace network
