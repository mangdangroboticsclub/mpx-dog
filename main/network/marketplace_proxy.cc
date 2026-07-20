#include "network/marketplace_proxy.h"
#include "network/wifi_sta.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "sdkconfig.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

static const char *TAG = "marketplace_proxy";

namespace network {

/* ── Configuration from Kconfig ────────────────────────────── */
static constexpr const char *GATEWAY_HOST = CONFIG_APP_CHAT_SERVER_IP;
static constexpr int         GATEWAY_PORT = CONFIG_APP_CHAT_SERVER_PORT;

/* ── Buffer sizes ───────────────────────────────────────────── */
static constexpr size_t HTTP_HEADER_BUF = 2048;
static constexpr size_t HTTP_BODY_BUF   = 16384;

std::string gateway_request(const char *method,
                            const char *path,
                            const std::string &body,
                            bool &ok)
{
    ok = false;

    // ── Echo mode check ───────────────────────────────────────
    if (std::strcmp(GATEWAY_HOST, "echo") == 0) {
        ESP_LOGW(TAG, "Gateway is in echo mode — cannot proxy request");
        return {};
    }

    // ── Check STA connectivity ────────────────────────────────
    if (wifi_sta_get_state() != StaState::Connected) {
        ESP_LOGW(TAG, "STA not connected — cannot reach gateway");
        return {};
    }

    // ── Resolve hostname ──────────────────────────────────────
    struct addrinfo hints = {};
    struct addrinfo *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%d", GATEWAY_PORT);

    int err = getaddrinfo(GATEWAY_HOST, port_str, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolution failed for %s: %d", GATEWAY_HOST, err);
        return {};
    }

    // ── Create TCP socket ─────────────────────────────────────
    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        freeaddrinfo(res);
        return {};
    }

    // ── Set timeouts ──────────────────────────────────────────
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // ── Connect ───────────────────────────────────────────────
    ESP_LOGI(TAG, "Connecting to gateway %s:%d ...", GATEWAY_HOST, GATEWAY_PORT);
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect to gateway failed");
        close(sock);
        freeaddrinfo(res);
        return {};
    }
    freeaddrinfo(res);

    // ── Build HTTP request ────────────────────────────────────
    std::string request;
    request.reserve(512 + body.size());

    bool has_body = !body.empty() &&
                    (std::strcmp(method, "POST") == 0 ||
                     std::strcmp(method, "PATCH") == 0 ||
                     std::strcmp(method, "PUT") == 0);

    request += std::string(method) + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + std::string(GATEWAY_HOST) + ":" + port_str + "\r\n";
    request += "Connection: close\r\n";
    request += "Accept: application/json\r\n";
    if (has_body) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    if (has_body) {
        request += body;
    }

    // ── Send request ──────────────────────────────────────────
    size_t total_sent = 0;
    while (total_sent < request.size()) {
        int n = send(sock, request.data() + total_sent,
                     request.size() - total_sent, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "Send failed");
            close(sock);
            return {};
        }
        total_sent += n;
    }

    // ── Read response ─────────────────────────────────────────
    std::string response;
    response.reserve(HTTP_BODY_BUF);
    char buf[512];
    int n;

    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = 0;
        response.append(buf, n);
    }

    close(sock);

    if (response.empty()) {
        ESP_LOGW(TAG, "Empty response from gateway");
        return {};
    }

    // ── Parse HTTP status line ────────────────────────────────
    // Look for "HTTP/1.1 200" or similar
    if (response.size() >= 12) {
        // Find the status code after the first space
        const char *start = response.data();
        const char *space1 = std::strchr(start, ' ');
        if (space1) {
            int status_code = std::atoi(space1 + 1);
            ok = (status_code >= 200 && status_code < 300);
            ESP_LOGI(TAG, "Gateway HTTP status: %d for %s %s",
                     status_code, method, path);
        } else {
            ESP_LOGW(TAG, "Could not find HTTP status in response: %.*s",
                     (int)std::min(response.size(), size_t(128)),
                     response.c_str());
        }
    } else {
        ESP_LOGW(TAG, "Response too short for HTTP status: %zu bytes",
                 response.size());
    }

    // ── Extract body (after "\r\n\r\n") ───────────────────────
    const char *header_end = std::strstr(response.data(), "\r\n\r\n");
    if (header_end) {
        std::string body_str(header_end + 4);
        ESP_LOGI(TAG, "Gateway response: %zu bytes (ok=%d)", body_str.size(), ok);
        return body_str;
    }

    // No header/body split — return whole response
    ESP_LOGI(TAG, "Gateway response: %zu bytes (no header split, ok=%d)",
             response.size(), ok);
    return response;
}

}  // namespace network
