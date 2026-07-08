#include "network/chat_ws.h"
#include "network/crypto.h"
#include "network/wifi_sta.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/tcp.h"
#include <sys/poll.h>
#include "mbedtls/sha256.h"
#include "cJSON.h"

#include "sdkconfig.h"

#include "robot/robot.h"
#include "lua/lua_vm.h"

static const char *TAG = "chat_ws";

namespace network {
namespace {

/* ── Configuration from Kconfig ────────────────────────────── */
static constexpr bool IS_ECHO_MODE() {
    return std::strcmp(CONFIG_APP_CHAT_SERVER_IP, "echo") == 0;
}
static constexpr const char *SERVER_IP   = CONFIG_APP_CHAT_SERVER_IP;
static constexpr int    SERVER_PORT   = CONFIG_APP_CHAT_SERVER_PORT;
static constexpr const char *WS_PATH     = CONFIG_APP_CHAT_WS_PATH;
static constexpr const char *ROBOT_UUID  = CONFIG_APP_ROBOT_UUID;

/* ── AES-256-GCM key (parsed from Kconfig hex string) ──────── */
static uint8_t s_aes_key[crypto::KEY_SIZE] = {};
static bool    s_key_loaded = false;

/* ── Upstream WebSocket client state ───────────────────────── */
static int      s_ws_sock = -1;           // TCP socket fd, -1 = not connected
static bool     s_upstream_connected = false;
static SemaphoreHandle_t s_sock_mutex = nullptr;  // protects s_ws_sock

/* ── Downstream reply queue (chat_send → async reply bridge) ─ */
static QueueHandle_t s_reply_queue = nullptr;

/* ═══════════════════════════════════════════════════════════════
 *  OpenClaw Permission System
 * ═══════════════════════════════════════════════════════════════
 *
 * Synchronisation between Lua worker task (requesting permission)
 * and WebSocket handler task (receiving user response).
 */
// Binary semaphore: given by WS handler when permission response arrives.
static SemaphoreHandle_t s_perm_semaphore = nullptr;
// Result of the pending permission request.
static bool     s_perm_approved = false;
// Unique action ID for the pending request (used to match response).
static char     s_perm_action_id[64] = {};
// Mutex protecting the permission state.
static SemaphoreHandle_t s_perm_mutex = nullptr;


/* ── PWA WebSocket client entry ──────────────────────────── */
struct PwaWsClient {
    httpd_req_t *req = nullptr;
    int fd = -1;                // socket fd for cross-task sends
    bool reply_ready = false;
    char session_id[64] = {};
    char pending_reply[2048] = {};
    // WebSocket fragmentation reassembly buffer
    char frag_buf[4096] = {};
    size_t frag_len = 0;
    bool fragmenting = false;
};

static constexpr size_t MAX_PWA_WS_CLIENTS = 4;

/* ── PWA WebSocket client pool ─────────────────────────────── */
static PwaWsClient s_pwa_clients[MAX_PWA_WS_CLIENTS] = {};
static SemaphoreHandle_t s_pwa_mutex = nullptr;
static httpd_handle_t s_httpd_handle = nullptr;
static constexpr size_t REPLY_TEXT_MAX = 2048;

/* ── PWA keepalive is handled via TCP keepalive + timeouts ──
 *
 * We do NOT send application-level WebSocket PING frames because
 * the browser's PONG response (masked per RFC 6455) can desync
 * the ESP-IDF HTTP server's internal WS frame parser, causing
 * subsequent frames to be rejected with "not properly masked".
 *
 * Instead we rely on:
 *   - TCP keepalive (10 s idle, 3 s interval, 3 probes) set in
 *     chat_ws_handler's HTTP_GET path
 *   - recv_wait_timeout=60  (configured in http_server.cc)
 *   - max_open_sockets=12   (configured in http_server.cc)
 */

/* ── Forward declarations for PWA client helpers ──────────── */
static PwaWsClient *get_pwa_client(httpd_req_t *req);
static void release_pwa_client(httpd_req_t *req);
static bool send_to_pwa_client(PwaWsClient *client, const char *json_text);
static void broadcast_to_pwa(const char *session_id, const char *json_text);

/* ── WebSocket protocol helpers ───────────────────────────────
 *
 * The upstream link is a raw (non-TLS) WebSocket connection.
 * Frames sent from the client (robot) MUST be masked per RFC 6455.
 */

/**
 * @brief Perform the WebSocket opening handshake on an established TCP socket.
 *
 * Sends the HTTP Upgrade request and verifies the 101 Switching Protocols
 * response, including the Sec-WebSocket-Accept hash.
 */
static bool ws_handshake(int sock, const char *host, int port, const char *path)
{
    char req[512];
    int n = std::snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, port);

    if (n <= 0 || (size_t)n >= sizeof(req)) {
        ESP_LOGE(TAG, "Handshake request too long");
        return false;
    }

    // Send the HTTP upgrade request
    int sent = send(sock, req, n, 0);
    if (sent != n) {
        ESP_LOGE(TAG, "Handshake send failed (sent=%d, expected=%d)", sent, n);
        return false;
    }

    // Read the response (expect "HTTP/1.1 101 Switching Protocols")
    char resp[256];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int r = recv(sock, resp + total, 1, 0);
        if (r <= 0) {
            ESP_LOGE(TAG, "Handshake recv failed");
            return false;
        }
        total += r;
        resp[total] = 0;
        // Stop at the end of headers
        if (total >= 4 && std::strstr(resp, "\r\n\r\n")) break;
    }
    resp[total] = 0;

    // Check for 101 response
    if (!std::strstr(resp, "101")) {
        ESP_LOGE(TAG, "Handshake failed, response: %s", resp);
        return false;
    }

    ESP_LOGI(TAG, "WebSocket handshake complete");
    return true;
}

/**
 * @brief Send a binary WebSocket frame (masked, per RFC 6455 §5.1).
 *
 * Client frames MUST be masked. Uses a random 4-byte mask key.
 */
static bool ws_send_binary(int sock, const uint8_t *data, size_t len)
{
    if (sock < 0) return false;

    // Build frame header. Max payload fits in 2-byte extended length.
    uint8_t header[4 + 2 + 4] = {};  // header + possible 2-byte ext len + mask key
    size_t hdr_len = 0;

    // FIN + opcode 0x02 (binary)
    header[hdr_len++] = 0x80 | 0x02;

    // Payload length with masking bit (0x80)
    if (len < 126) {
        header[hdr_len++] = 0x80 | (uint8_t)len;
    } else if (len <= 65535) {
        header[hdr_len++] = 0x80 | 126;
        header[hdr_len++] = (uint8_t)(len >> 8);
        header[hdr_len++] = (uint8_t)(len & 0xFF);
    } else {
        // 64-bit extended length — not needed for chat messages
        return false;
    }

    // Masking key (4 random bytes)
    uint8_t mask_key[4];
    crypto::random_bytes(mask_key, 4);
    for (size_t i = 0; i < 4; ++i) header[hdr_len++] = mask_key[i];

    // Send header
    if (send(sock, header, hdr_len, 0) != (int)hdr_len) {
        return false;
    }

    // Send masked payload in chunks to avoid large stack allocations
    uint8_t masked[256];
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > sizeof(masked)) ? sizeof(masked) : (len - offset);
        for (size_t i = 0; i < chunk; ++i) {
            masked[i] = data[offset + i] ^ mask_key[(offset + i) & 3];
        }
        if (send(sock, masked, chunk, 0) != (int)chunk) {
            return false;
        }
        offset += chunk;
    }

    return true;
}

/**
 * @brief Read exactly @p len bytes from a socket, looping until all bytes arrive.
 *
 * On a TCP socket, a single recv() may return fewer bytes than requested.
 * This helper keeps calling recv() until all requested bytes are received
 * or an error / disconnect occurs.
 */
static bool recv_all(int sock, uint8_t *buf, size_t len)
{
    while (len > 0) {
        int r = recv(sock, buf, len, 0);
        if (r <= 0) return false;
        buf  += r;
        len  -= (size_t)r;
    }
    return true;
}

/**
 * @brief Receive a WebSocket frame, extracting the payload.
 *
 * @param sock     Socket fd.
 * @param buf      Output buffer for payload.
 * @param buf_size Size of output buffer.
 * @param out_len  Populated with actual payload length.
 * @param out_type Populated with frame opcode.
 * @return true on success.
 */
static bool ws_recv_frame(int sock, uint8_t *buf, size_t buf_size,
                           size_t &out_len, uint8_t &out_type)
{
    out_len = 0;
    out_type = 0;

    // Read first 2 bytes (FIN+opcode, mask+length)
    uint8_t hdr[2];
    if (!recv_all(sock, hdr, 2)) return false;

    out_type = hdr[0] & 0x0F;   // opcode
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    // Extended payload length
    if (payload_len == 126) {
        uint8_t ext[2];
        if (!recv_all(sock, ext, 2)) return false;
        payload_len = ((uint16_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (!recv_all(sock, ext, 8)) return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) payload_len = (payload_len << 8) | ext[i];
    }

    // Masking key (server → client frames are NOT masked per RFC 6455,
    // but we handle both for robustness)
    uint8_t mask_key[4] = {};
    if (masked) {
        if (!recv_all(sock, mask_key, 4)) return false;
    }

    // Read payload
    if (payload_len > buf_size) {
        ESP_LOGW(TAG, "WS frame too large: %llu > %zu", payload_len, buf_size);
        // Drain and discard
        uint8_t discard[128];
        uint64_t remaining = payload_len;
        while (remaining > 0) {
            size_t chunk = (remaining > sizeof(discard)) ? sizeof(discard) : (size_t)remaining;
            if (!recv_all(sock, discard, chunk)) return false;
            remaining -= chunk;
        }
        return false;
    }

    size_t read_total = 0;
    while (read_total < payload_len) {
        int r = recv(sock, buf + read_total, (int)(payload_len - read_total), 0);
        if (r <= 0) return false;
        read_total += r;
    }

    // Unmask if needed
    if (masked) {
        for (size_t i = 0; i < payload_len; ++i) {
            buf[i] ^= mask_key[i & 3];
        }
    }

    out_len = (size_t)payload_len;
    return true;
}

/**
 * @brief Close the WebSocket connection gracefully.
 */
static void ws_close(int sock)
{
    if (sock < 0) return;
    // Send close frame (opcode 0x08)
    uint8_t close_frame[2] = {0x88, 0x80};  // FIN + close, masked, length 0
    uint8_t mask_key[4];
    crypto::random_bytes(mask_key, 4);
    // Combine header + mask key into a single send for atomicity
    uint8_t close_pkt[6];
    std::memcpy(close_pkt, close_frame, 2);
    std::memcpy(close_pkt + 2, mask_key, 4);
    send(sock, close_pkt, 6, 0);
    // Shutdown and close
    shutdown(sock, SHUT_RDWR);
    close(sock);
}

/**
 * @brief Send a WebSocket ping frame (opcode 0x09, masked, empty payload).
 *
 * Per RFC 6455 §5.5.2, ping frames from the client MUST be masked.
 */
static bool ws_send_ping(int sock)
{
    if (sock < 0) return false;

    // Frame: FIN=1, opcode=0x09 (ping), MASK=1, length=0
    uint8_t frame[6];
    frame[0] = 0x89;        // FIN + ping opcode
    frame[1] = 0x80;        // MASK + payload length 0

    // 4-byte mask key (required for client frames)
    crypto::random_bytes(frame + 2, 4);

    int sent = send(sock, frame, sizeof(frame), 0);
    return sent == sizeof(frame);
}

/**
 * @brief Send a WebSocket pong frame (opcode 0x0A, masked) echoing a payload.
 *
 * Per RFC 6455 §5.5.2, a pong sent in response to a ping MUST have identical
 * application data as the ping.  The websockets Python library (v16+) relies
 * on this — it sends PINGs with random payload and validates the echo.
 */
static bool ws_send_pong(int sock, const uint8_t *payload, size_t payload_len)
{
    if (sock < 0) return false;

    // Build frame header. Max payload fits in 2-byte extended length.
    uint8_t header[4 + 2 + 4] = {};  // header + possible 2-byte ext len + mask key
    size_t hdr_len = 0;

    // FIN + opcode 0x0A (pong)
    header[hdr_len++] = 0x80 | 0x0A;

    // Payload length with masking bit (0x80)
    if (payload_len < 126) {
        header[hdr_len++] = 0x80 | (uint8_t)payload_len;
    } else {
        return false;  // PONG payloads are tiny in practice
    }

    // Masking key (4 random bytes)
    uint8_t mask_key[4];
    crypto::random_bytes(mask_key, 4);
    for (size_t i = 0; i < 4; ++i) header[hdr_len++] = mask_key[i];

    // Send header
    if (send(sock, header, hdr_len, 0) != (int)hdr_len) {
        return false;
    }

    // Send masked payload in chunks
    uint8_t masked[128];
    size_t offset = 0;
    while (offset < payload_len) {
        size_t chunk = (payload_len - offset > sizeof(masked))
                           ? sizeof(masked)
                           : (payload_len - offset);
        for (size_t i = 0; i < chunk; ++i) {
            masked[i] = payload[offset + i] ^ mask_key[(offset + i) & 3];
        }
        if (send(sock, masked, chunk, 0) != (int)chunk) {
            return false;
        }
        offset += chunk;
    }

    return true;
}

/* ── Lua command execution queue (offloaded from upstream task) ─ */
struct LuaCommand {
    char script[1024];       // Lua source (truncated if too long)
    char session_id[64];     // Session context for step feedback
    int  seq;                // Sequence number within command array
    int  total;              // Total commands in the array
};

static QueueHandle_t s_lua_queue = nullptr;

// Forward declarations for functions used by lua_worker_task
// (defined later in this file)
static std::string json_escape(const std::string &s);
static uint8_t *build_upstream_frame(const char *json_text,
                                      size_t json_len,
                                      size_t &out_frame_len);
static bool send_upstream_frame(const uint8_t *frame, size_t len);

/**
 * @brief Worker task that processes Lua scripts from the queue.
 *
 * Runs on a separate stack so that long-running or multi-step Lua
 * scripts do NOT block the upstream WebSocket receive loop.
 * Emits step messages to the PWA for each executed command.
 */
static void lua_worker_task(void *arg)
{
    (void)arg;

    // Allocate buffers on the heap to avoid stack overflow.
    // The lua_worker_task stack must accommodate lua_run_string()
    // internals, so keep the task stack lean.
    auto *cmd = static_cast<LuaCommand *>(std::malloc(sizeof(LuaCommand)));
    auto *step_json = static_cast<char *>(std::malloc(1536));
    auto *lua_output = static_cast<char *>(std::malloc(512));
    auto *done_json = static_cast<char *>(std::malloc(2048));

    if (!cmd || !step_json || !lua_output || !done_json) {
        ESP_LOGE(TAG, "OOM in lua_worker_task");
        std::free(cmd);
        std::free(step_json);
        std::free(lua_output);
        std::free(done_json);
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        if (xQueueReceive(s_lua_queue, cmd, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Lua worker executing [%d/%d]: %s",
                     cmd->seq, cmd->total, cmd->script);

            // ── Send step message to PWA before execution ──────
            int step_len = std::snprintf(step_json, 1536,
                R"({"type":"step","text":"Executing Lua: %s","seq":%d,"total":%d,"session_id":"%s"})",
                cmd->script, cmd->seq, cmd->total, cmd->session_id);
            if (step_len > 0) {
                broadcast_to_pwa(cmd->session_id, step_json);
            }

            // ── Execute the Lua script ─────────────────────────
            esp_err_t lua_ret = lua_run_string(
                cmd->script,
                lua_output, 512,
                5000  // 5 second timeout
            );

            // ── Send completion step with output ───────────────
            const char *status = "completed";
            if (lua_ret == ESP_ERR_TIMEOUT) {
                status = "timeout";
                ESP_LOGW(TAG, "  → Lua script timed out [%d/%d]", cmd->seq, cmd->total);
            } else if (lua_ret != ESP_OK) {
                status = "error";
                ESP_LOGW(TAG, "  → Lua script error [%d/%d]: %s",
                         cmd->seq, cmd->total, lua_output);
            } else if (std::strlen(lua_output) > 0) {
                ESP_LOGI(TAG, "  → Lua output [%d/%d]: %s",
                         cmd->seq, cmd->total, lua_output);
            }

            // ── Send command result to PWA (shown as action under bot msg) ─
            {
                std::string escaped_script = json_escape(std::string(cmd->script));
                std::string escaped_output = json_escape(std::string(lua_output));
                int done_len = std::snprintf(done_json, 2048,
                    R"({"type":"command_result","script":"%s","output":"%s","status":"%s","session_id":"%s"})",
                    escaped_script.c_str(), escaped_output.c_str(),
                    status, cmd->session_id);
                if (done_len > 0) {
                    broadcast_to_pwa(cmd->session_id, done_json);
                }
            }

            // ── Send Lua output upstream as a user chat message ─────
            // This uses the same pipeline as normal user chat messages,
            // so OpenClaw sees "LUA: <output>" as a new message from the
            // user and can incorporate the result into its reasoning.
            if (cmd->session_id[0] != 0 &&
                (lua_output[0] != 0 || lua_ret != ESP_OK)) {
                std::string escaped_output = json_escape(std::string(lua_output));
                char upstream_json[1536];
                int64_t now_ts = esp_timer_get_time() / 1000000;
                int up_len = std::snprintf(upstream_json, sizeof(upstream_json),
                    R"({"type":"user_chat_input","text":"LUA: %s","session_id":"%s","ts":%lld})",
                    escaped_output.c_str(), cmd->session_id,
                    (long long)now_ts);

                if (up_len > 0) {
                    size_t frame_len = 0;
                    uint8_t *frame = build_upstream_frame(
                        upstream_json, static_cast<size_t>(up_len), frame_len);

                    if (frame) {
                        // Send with mutex protection to avoid concurrent
                        // socket writes with the upstream task
                        xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
                        bool sent = send_upstream_frame(frame, frame_len);
                        xSemaphoreGive(s_sock_mutex);

                        if (sent) {
                            ESP_LOGI(TAG, "  → Lua output sent upstream as chat msg (session=%s)",
                                     cmd->session_id);
                        }
                        std::free(frame);
                    }
                }
            }
        }
    }
}

/* ── Helper: read AES key from Kconfig hex string ──────────── */
static bool load_aes_key()
{
    if (s_key_loaded) return true;

    if (!crypto::hex_to_bytes(CONFIG_APP_CHAT_AES_KEY_HEX,
                               s_aes_key, crypto::KEY_SIZE)) {
        ESP_LOGE(TAG, "Failed to parse AES key from Kconfig");
        return false;
    }

    s_key_loaded = true;
    ESP_LOGI(TAG, "AES-256-GCM key loaded (%d bytes)", crypto::KEY_SIZE);
    return true;
}

/* ── Build a binary upstream frame ───────────────────────────
 *
 * Layout per SDD §2.2:
 *   [0:16]   robot_uuid (plain-text AAD)
 *   [16:28]  IV nonce (12 random bytes)
 *   [28:-16] AES-256-GCM ciphertext of the JSON payload
 *   [-16:]   16-byte authentication tag
 */
static uint8_t *build_upstream_frame(const char *json_text,
                                      size_t json_len,
                                      size_t &out_frame_len)
{
    // Frame = header + ciphertext + tag
    out_frame_len = BINARY_FRAME_HEADER_SIZE + json_len + BINARY_FRAME_TAG_SIZE;
    auto *frame = static_cast<uint8_t *>(std::malloc(out_frame_len));
    if (!frame) {
        ESP_LOGE(TAG, "OOM allocating upstream frame (%zu bytes)", out_frame_len);
        out_frame_len = 0;
        return nullptr;
    }

    auto *hdr = reinterpret_cast<BinaryFrameHeader *>(frame);

    // ── Fill robot_uuid (pad or truncate to 16 bytes) ──────────
    std::memset(hdr->robot_uuid, 0, 16);
    std::strncpy(reinterpret_cast<char *>(hdr->robot_uuid),
                 ROBOT_UUID, 16);

    // ── Generate random IV ─────────────────────────────────────
    crypto::random_bytes(hdr->iv, crypto::IV_SIZE);

    // ── Encrypt with AAD = robot_uuid (16 bytes) ───────────────
    uint8_t *ciphertext = frame + BINARY_FRAME_HEADER_SIZE;
    uint8_t *tag = ciphertext + json_len;

    bool ok = crypto::gcm_encrypt(
        s_aes_key,
        hdr->iv,
        hdr->robot_uuid, 16,                         // AAD
        reinterpret_cast<const uint8_t *>(json_text), json_len,
        ciphertext,
        tag);

    if (!ok) {
        ESP_LOGE(TAG, "Encryption failed, dropping frame");
        std::free(frame);
        out_frame_len = 0;
        return nullptr;
    }

    return frame;
}

/* ── Send raw bytes over the upstream (cloud) WebSocket ────── */
static bool send_upstream_frame(const uint8_t *frame, size_t len)
{
    if (!frame || len == 0) return false;

    if (IS_ECHO_MODE()) {
        return false;
    }

    if (!s_upstream_connected || s_ws_sock < 0) {
        ESP_LOGW(TAG, "Upstream not connected — dropping %zu bytes", len);
        return false;
    }

    // Send as a binary WebSocket frame (masked per RFC 6455)
    bool ok = ws_send_binary(s_ws_sock, frame, len);
    if (ok) {
        ESP_LOGI(TAG, "Upstream frame sent (%zu bytes)", len);
    } else {
        ESP_LOGE(TAG, "Failed to send upstream frame");
    }
    return ok;
}

/* ── JSON string escaper ─────────────────────────────────────
 *
 * Escapes a string so it can be safely embedded inside a JSON
 * string literal (between double quotes).  Handles ", \, newline,
 * tab, carriage return, and other control characters.
 */
static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/* ── Parse a downstream response from the cloud ──────────────
 *
 * The downstream response is an encrypted binary frame.
 * After decryption, the plaintext is a JSON object:
 * {
 *   "type": "chat_reply",
 *   "text": "I've verified the lab doors are locked.",
 *   "actions": [
 *     {"gait": "none", "param": 0}
 *   ]
 * }
 */
static bool process_downstream_frame(const uint8_t *frame, size_t len)
{
    if (len < BINARY_FRAME_HEADER_SIZE + BINARY_FRAME_TAG_SIZE) {
        ESP_LOGW(TAG, "Downstream frame too short (%zu bytes)", len);
        return false;
    }

    auto *hdr = reinterpret_cast<const BinaryFrameHeader *>(frame);
    size_t ct_len = len - BINARY_FRAME_HEADER_SIZE - BINARY_FRAME_TAG_SIZE;

    // Allocate plaintext buffer
    auto *plaintext = static_cast<uint8_t *>(std::malloc(ct_len + 1));
    if (!plaintext) {
        ESP_LOGE(TAG, "OOM for downstream plaintext");
        return false;
    }

    const uint8_t *ciphertext = frame + BINARY_FRAME_HEADER_SIZE;
    const uint8_t *tag = ciphertext + ct_len;

    bool ok = crypto::gcm_decrypt(
        s_aes_key,
        hdr->iv,
        hdr->robot_uuid, 16,   // AAD
        ciphertext, ct_len,
        tag,
        plaintext);

    if (!ok) {
        ESP_LOGW(TAG, "Downstream frame decryption FAILED (tampered?)");
        std::free(plaintext);
        return false;
    }

    plaintext[ct_len] = 0;
    ESP_LOGI(TAG, "Downstream decrypted: %s", (const char *)plaintext);

    // ── Parse the JSON response with cJSON ─────────────────────
    // cJSON handles all whitespace variations, Unicode escapes,
    // and JSON formatting differences from any upstream LLM.
    cJSON *root = cJSON_Parse((const char *)plaintext);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse downstream JSON");
        std::free(plaintext);
        return false;
    }

    // ── Determine message type ──────────────────────────────────
    const cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        std::free(plaintext);
        return false;
    }
    const std::string msg_type = type_item->valuestring;

    // Shared helper: get a string field, defaulting to ""
    auto json_str = [&](const char *key) -> std::string {
        const cJSON *item = cJSON_GetObjectItem(root, key);
        return (item && cJSON_IsString(item)) ? item->valuestring : "";
    };
    // Shared helper: get an int field, defaulting to @p def
    auto json_int = [&](const char *key, int def = 0) -> int {
        const cJSON *item = cJSON_GetObjectItem(root, key);
        return (item && cJSON_IsNumber(item)) ? item->valueint : def;
    };

    if (msg_type == "step") {
        // ── Step message — forward to PWA immediately ─────────
        std::string step_text = json_str("text");
        int seq  = json_int("seq", 1);
        int total = json_int("total", 1);
        std::string session_id = json_str("session_id");

        std::string step_escaped = json_escape(step_text);
        char step_json[1536];
        int len = std::snprintf(step_json, sizeof(step_json),
            R"({"type":"step","text":"%s","seq":%d,"total":%d,"session_id":"%s"})",
            step_escaped.c_str(), seq, total, session_id.c_str());
        if (len > 0) {
            broadcast_to_pwa(session_id.c_str(), step_json);
        }
        ESP_LOGI(TAG, "  → Step [%d/%d]: %s", seq, total, step_text.c_str());

    } else if (msg_type == "chat_reply") {
        // ── Chat reply — extract text, commands, forward to PWA ─
        std::string reply_text = json_str("text");
        std::string session_id = json_str("session_id");

        // Push reply text to the queue so HTTP handlers can pick it up
        if (s_reply_queue && !reply_text.empty()) {
            char reply_buf[REPLY_TEXT_MAX];
            size_t copy_len = reply_text.copy(reply_buf, REPLY_TEXT_MAX - 1);
            reply_buf[copy_len] = 0;
            xQueueSend(s_reply_queue, reply_buf, pdMS_TO_TICKS(10));
        }

        // Extract Lua commands from the "commands" array
        const cJSON *cmds = cJSON_GetObjectItem(root, "commands");
        int cmd_count = 0;
        int total_cmds = cJSON_GetArraySize(cmds);

        if (cmds && cJSON_IsArray(cmds)) {
            for (int i = 0; i < total_cmds; ++i) {
                const cJSON *cmd_obj = cJSON_GetArrayItem(cmds, i);
                if (!cmd_obj) continue;

                const cJSON *type_item = cJSON_GetObjectItem(cmd_obj, "type");
                if (!type_item || !cJSON_IsString(type_item)) continue;
                if (std::strcmp(type_item->valuestring, "lua") != 0) continue;

                const cJSON *script_item = cJSON_GetObjectItem(cmd_obj, "script");
                if (!script_item || !cJSON_IsString(script_item)) continue;

                cmd_count++;
                const char *script_text = script_item->valuestring;
                ESP_LOGI(TAG, "  → Lua command [%d/%d]: %s",
                         cmd_count, total_cmds, script_text);

                if (s_lua_queue) {
                    LuaCommand lua_cmd = {};
                    size_t copy_len = std::strlen(script_text);
                    if (copy_len >= sizeof(lua_cmd.script))
                        copy_len = sizeof(lua_cmd.script) - 1;
                    std::memcpy(lua_cmd.script, script_text, copy_len);
                    lua_cmd.script[copy_len] = 0;
                    lua_cmd.seq = cmd_count;
                    lua_cmd.total = total_cmds;
                    std::strncpy(lua_cmd.session_id,
                                 session_id.c_str(),
                                 sizeof(lua_cmd.session_id) - 1);
                    if (xQueueSend(s_lua_queue, &lua_cmd,
                                   pdMS_TO_TICKS(10)) != pdTRUE) {
                        ESP_LOGW(TAG, "  → Lua queue full, dropping script");
                    }
                }
            }
        }

        // Forward full reply to PWA via broadcast — chunked
        // to avoid large buffers and TCP send pressure.
        //
        // If the escaped text fits in ~2 KB, send a single chat_reply.
        // Otherwise split into chat_reply_chunk messages (seq/total)
        // that the PWA reassembles on the receiving end.
        std::string escaped = json_escape(reply_text);
        constexpr size_t SINGLE_MAX = 2048;
        constexpr size_t CHUNK_TEXT = 1536;   // text bytes per chunk
        constexpr size_t CHUNK_BUF = 2048;    // total JSON buffer per chunk

        // Quick check: total length if sent as one message
        size_t single_len = escaped.size() + 120;  // rough JSON overhead
        if (single_len < SINGLE_MAX) {
            // ── Single message — no chunking needed ─────────
            char buf[SINGLE_MAX];
            int n = std::snprintf(buf, sizeof(buf),
                R"({"type":"chat_reply","text":"%s","commands":[],"session_id":"%s"})",
                escaped.c_str(), session_id.c_str());
            if (n > 0) broadcast_to_pwa(session_id.c_str(), buf);
        } else {
            // ── Chunked delivery ─────────────────────────────
            size_t total = (escaped.size() + CHUNK_TEXT - 1) / CHUNK_TEXT;
            ESP_LOGI(TAG, "Splitting chat_reply into %zu chunks", total);
            for (size_t seq = 0; seq < total; ++seq) {
                size_t off = seq * CHUNK_TEXT;
                size_t len = std::min(CHUNK_TEXT, escaped.size() - off);
                std::string piece = escaped.substr(off, len);

                char buf[CHUNK_BUF];
                int n = std::snprintf(buf, sizeof(buf),
                    R"({"type":"chat_reply_chunk","text":"%s","seq":%zu,"total":%zu,"session_id":"%s"})",
                    piece.c_str(), seq, total, session_id.c_str());
                if (n > 0) broadcast_to_pwa(session_id.c_str(), buf);
            }
        }
    }

    cJSON_Delete(root);
    std::free(plaintext);
    return true;
}

/* ── PWA WebSocket client management ───────────────────────── */

PwaWsClient *get_pwa_client(httpd_req_t *req)
{
    if (!s_pwa_mutex) return nullptr;
    xSemaphoreTake(s_pwa_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_PWA_WS_CLIENTS; ++i) {
        if (s_pwa_clients[i].req == nullptr) {
            s_pwa_clients[i].req = req;
            s_pwa_clients[i].fd = httpd_req_to_sockfd(req);
            s_pwa_clients[i].reply_ready = false;
            s_pwa_clients[i].session_id[0] = 0;
            s_pwa_clients[i].pending_reply[0] = 0;
            s_httpd_handle = req->handle;
            xSemaphoreGive(s_pwa_mutex);
            return &s_pwa_clients[i];
        }
    }
    xSemaphoreGive(s_pwa_mutex);
    ESP_LOGW(TAG, "All PWA WS client slots full");
    return nullptr;
}

void release_pwa_client(httpd_req_t *req)
{
    if (!s_pwa_mutex) return;
    xSemaphoreTake(s_pwa_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_PWA_WS_CLIENTS; ++i) {
        if (s_pwa_clients[i].req == req) {
            s_pwa_clients[i].req = nullptr;
            s_pwa_clients[i].fd = -1;
            s_pwa_clients[i].reply_ready = false;
            s_pwa_clients[i].session_id[0] = 0;
            s_pwa_clients[i].pending_reply[0] = 0;
            s_pwa_clients[i].fragmenting = false;
            s_pwa_clients[i].frag_len = 0;
            break;
        }
    }
    xSemaphoreGive(s_pwa_mutex);
}

bool send_to_pwa_client(PwaWsClient *client, const char *json_text)
{
    if (!client || client->fd < 0 || !json_text || !s_httpd_handle) return false;

    httpd_ws_frame_t resp_pkt{};
    resp_pkt.payload = const_cast<uint8_t *>(
        reinterpret_cast<const uint8_t *>(json_text));
    resp_pkt.len = std::strlen(json_text);
    resp_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Use httpd_ws_send_data which queues the work to the HTTP server task
    // via httpd_queue_work internally, making it safe to call from any task.
    // The calling task blocks until the send completes, so the stack-allocated
    // json_text payload remains valid.
    esp_err_t ret = httpd_ws_send_data(s_httpd_handle, client->fd, &resp_pkt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send to PWA WS: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

void broadcast_to_pwa(const char *session_id, const char *json_text)
{
    if (!s_pwa_mutex) return;
    xSemaphoreTake(s_pwa_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_PWA_WS_CLIENTS; ++i) {
        if (s_pwa_clients[i].fd < 0) continue;
        // If session_id specified, only send to matching clients
        if (session_id && session_id[0] != 0 &&
            std::strcmp(s_pwa_clients[i].session_id, session_id) != 0) {
            continue;
        }
        if (!send_to_pwa_client(&s_pwa_clients[i], json_text)) {
            // Send failed — client is gone.  Release the slot so we
            // don't keep trying to push messages to a dead connection.
            ESP_LOGI(TAG, "Removing stale PWA client slot %zu", i);
            s_pwa_clients[i].req = nullptr;
            s_pwa_clients[i].fd = -1;
            s_pwa_clients[i].reply_ready = false;
            s_pwa_clients[i].session_id[0] = 0;
            s_pwa_clients[i].pending_reply[0] = 0;
        }
    }
    xSemaphoreGive(s_pwa_mutex);
}

/* ── Upstream connection management task ─────────────────────
 *
 * Connects to the cloud ingress server via WebSocket, sends
 * encrypted frames upstream, and receives downstream frames.
 * Auto-reconnects on disconnect.
 */
static void upstream_task(void *arg)
{
    (void)arg;

    // Allocate receive buffer on the heap (avoids stack overflow)
    auto *recv_buf = static_cast<uint8_t *>(std::malloc(4096));
    if (!recv_buf) {
        ESP_LOGE(TAG, "OOM for recv buffer");
        vTaskDelete(nullptr);
        return;
    }

    if (IS_ECHO_MODE()) {
        ESP_LOGI(TAG, "Upstream task: echo mode — no cloud connection needed");
        std::free(recv_buf);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Upstream task: will connect to %s:%d%s",
             SERVER_IP, SERVER_PORT, WS_PATH);

    while (true) {
        // ── Wait for STA connectivity ──────────────────────────
        while (wifi_sta_get_state() != StaState::Connected) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        // ── Resolve hostname ───────────────────────────────────
        struct addrinfo hints = {};
        struct addrinfo *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        char port_str[8];
        std::snprintf(port_str, sizeof(port_str), "%d", SERVER_PORT);

        int err = getaddrinfo(SERVER_IP, port_str, &hints, &res);
        if (err != 0 || !res) {
            ESP_LOGW(TAG, "DNS resolution failed for %s: %d", SERVER_IP, err);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // ── Create TCP socket ──────────────────────────────────
        int sock = socket(res->ai_family, res->ai_socktype, 0);
        if (sock < 0) {
            ESP_LOGW(TAG, "Failed to create socket");
            freeaddrinfo(res);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // ── Set timeouts ───────────────────────────────────────
        // Receive timeout is now shorter (30 s) — the outer poll() loop
        // handles keepalive pings, but a safety timeout on recv() prevents
        // ws_recv_frame from hanging indefinitely on a dead connection.
        struct timeval tv_recv = {.tv_sec = 30, .tv_usec = 0};
        struct timeval tv_send = {.tv_sec = 5,   .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));

        // ── Enable TCP keepalive ───────────────────────────────
        // This keeps the TCP layer alive even if the application
        // is temporarily blocked.  First probe after 10 s of idle,
        // then every 3 s, max 3 retries before declaring dead.
        int keepalive = 1;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        int keep_idle = 10;
        int keep_intvl = 3;
        int keep_cnt = 3;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));

        // ── Connect ────────────────────────────────────────────
        ESP_LOGI(TAG, "Connecting to %s:%d ...", SERVER_IP, SERVER_PORT);
        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGW(TAG, "TCP connect failed");
            close(sock);
            freeaddrinfo(res);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        freeaddrinfo(res);
        ESP_LOGI(TAG, "TCP connected");

        // ── WebSocket handshake ────────────────────────────────
        if (!ws_handshake(sock, SERVER_IP, SERVER_PORT, WS_PATH)) {
            ESP_LOGW(TAG, "WebSocket handshake failed");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // ── Connection established ─────────────────────────────
        xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
        s_ws_sock = sock;
        s_upstream_connected = true;
        xSemaphoreGive(s_sock_mutex);

        ESP_LOGI(TAG, "Upstream WebSocket connected to %s:%d%s",
                 SERVER_IP, SERVER_PORT, WS_PATH);

        // ── Receive loop (downstream frames) with keepalive ───
        //
        // We use poll() with a 30-second timeout so we can send
        // WebSocket ping frames periodically when the connection
        // is idle.  This prevents intermediate gateways / load
        // balancers from closing the connection due to inactivity.
        //
        // Additionally, we track wall-clock time since our last
        // PING and send one proactively every ~15 seconds even
        // when the server sends data frames (which reset poll).
        // This satisfies the server-side websockets library's
        // keepalive_ping() mechanism — the library sends PINGs
        // and expects PONGs echoed back; our proactive PING lets
        // it see bidirectional activity on the socket.
        int64_t last_ping_us = esp_timer_get_time();

        while (true) {
            struct pollfd pfd = {};
            pfd.fd = sock;
            pfd.events = POLLIN;

            int poll_ret = poll(&pfd, 1, 30000);   // 30 s timeout

            if (poll_ret < 0) {
                ESP_LOGW(TAG, "poll() error — connection lost");
                break;
            }

            if (poll_ret == 0) {
                // ── Timeout — send keepalive ping ─────────────
                if (!ws_send_ping(sock)) {
                    ESP_LOGW(TAG, "Keepalive ping failed — connection lost");
                    break;
                }
                ESP_LOGD(TAG, "Keepalive ping sent (poll timeout)");
                last_ping_us = esp_timer_get_time();
                continue;
            }

            // ── Data available — receive a frame ──────────────
            size_t frame_len = 0;
            uint8_t opcode = 0;

            if (!ws_recv_frame(sock, recv_buf, 4096,
                               frame_len, opcode)) {
                ESP_LOGW(TAG, "WS receive failed — connection lost");
                break;
            }

            // Handle opcodes
            if (opcode == 0x08) {
                // Close frame
                ESP_LOGI(TAG, "Server sent close frame");
                break;
            } else if (opcode == 0x09) {
                // Ping — reply with pong echoing the payload (RFC 6455 §5.5.2)
                ESP_LOGD(TAG, "PING received (%zu bytes payload)", frame_len);
                ws_send_pong(sock, recv_buf, frame_len);
                continue;
            } else if (opcode == 0x0A) {
                // Pong — ignore (could be response to our ping)
                continue;
            } else if (opcode == 0x02 || opcode == 0x01) {
                // Binary (0x02) or Text (0x01) — process as downstream
                ESP_LOGI(TAG, "Downstream frame received (%zu bytes, opcode=%d)",
                         frame_len, opcode);
                process_downstream_frame(recv_buf, frame_len);
            }

            // ── Proactive keepalive PING every ~15 s ──────────
            // The server-side websockets library expects regular
            // bidirectional activity.  If we only send data when
            // the user chats, the server's keepalive_ping timer
            // fires and the connection drops.  Send a PING here
            // regardless of what we received so the server sees a
            // healthy two-way link.
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_ping_us > 15000000) {  // 15 seconds
                if (ws_send_ping(sock)) {
                    ESP_LOGD(TAG, "Proactive keepalive PING sent");
                }
                last_ping_us = now_us;
            }
        }

        // ── Cleanup ────────────────────────────────────────────
        xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
        s_ws_sock = -1;
        s_upstream_connected = false;
        xSemaphoreGive(s_sock_mutex);

        ws_close(sock);
        ESP_LOGI(TAG, "Upstream disconnected — reconnecting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

}  // anonymous namespace

/* ═══════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════ */

bool init_chat_pipeline()
{
    // ── Create socket mutex ────────────────────────────────────
    s_sock_mutex = xSemaphoreCreateMutex();
    if (!s_sock_mutex) {
        ESP_LOGE(TAG, "Failed to create socket mutex");
        return false;
    }

    // ── Create PWA client mutex ────────────────────────────────
    s_pwa_mutex = xSemaphoreCreateMutex();
    if (!s_pwa_mutex) {
        ESP_LOGE(TAG, "Failed to create PWA mutex");
        return false;
    }

    // ── Create permission semaphore and mutex ──────────────────
    s_perm_semaphore = xSemaphoreCreateBinary();
    if (!s_perm_semaphore) {
        ESP_LOGE(TAG, "Failed to create permission semaphore");
        return false;
    }
    // Start in "not signalled" state
    xSemaphoreTake(s_perm_semaphore, 0);

    s_perm_mutex = xSemaphoreCreateMutex();
    if (!s_perm_mutex) {
        ESP_LOGE(TAG, "Failed to create permission mutex");
        return false;
    }

    // ── Create reply queue (chat_send → downstream bridge) ────
    s_reply_queue = xQueueCreate(16, REPLY_TEXT_MAX);
    if (!s_reply_queue) {
        ESP_LOGE(TAG, "Failed to create reply queue");
        return false;
    }

    // ── Load AES key ───────────────────────────────────────────
    if (!load_aes_key()) {
        ESP_LOGE(TAG, "Failed to load AES key — chat pipeline disabled");
        return false;
    }

    // ── No PWA application-level keepalive PING (see comment at top) ─
    // TCP keepalive + recv_wait_timeout + max_open_sockets are sufficient.

    // ── Create Lua command queue ───────────────────────────────
    s_lua_queue = xQueueCreate(8, sizeof(LuaCommand));
    if (!s_lua_queue) {
        ESP_LOGE(TAG, "Failed to create Lua command queue");
        return false;
    }

    // ── Spawn Lua worker task ──────────────────────────────────
    BaseType_t rv = xTaskCreatePinnedToCore(
        lua_worker_task, "chat_lua", 8192, nullptr, 8, nullptr, 0);
    if (rv != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Lua worker task");
        return false;
    }

    // ── Spawn upstream connection task ─────────────────────────
    rv = xTaskCreatePinnedToCore(
        upstream_task, "chat_upstream", 8192, nullptr, 8, nullptr, 0);
    if (rv != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upstream task");
        return false;
    }

    if (IS_ECHO_MODE()) {
        ESP_LOGI(TAG, "Chat pipeline initialised — ECHO mode (v2.0 streaming)");
    } else {
        ESP_LOGI(TAG, "Chat pipeline initialised — upstream: %s:%d%s",
                 SERVER_IP, SERVER_PORT, WS_PATH);
    }

    return true;
}

void pwa_release_fd(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (!s_pwa_mutex) return;
    xSemaphoreTake(s_pwa_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_PWA_WS_CLIENTS; ++i) {
        if (s_pwa_clients[i].fd == sockfd) {
            ESP_LOGD(TAG, "PWA client fd=%d closed, releasing slot %zu", sockfd, i);
            s_pwa_clients[i].req = nullptr;
            s_pwa_clients[i].fd = -1;
            s_pwa_clients[i].reply_ready = false;
            s_pwa_clients[i].session_id[0] = 0;
            s_pwa_clients[i].pending_reply[0] = 0;
            s_pwa_clients[i].fragmenting = false;
            s_pwa_clients[i].frag_len = 0;
            break;
        }
    }
    xSemaphoreGive(s_pwa_mutex);
}

/* ── REST handler: POST /v1/chat/send ─────────────────────── */

esp_err_t chat_send_handler(httpd_req_t *req)
{
    // ── Read request body (loop to handle partial reads) ─────────
    char buf[2048] = {};
    int offset = 0;
    while (offset < (int)sizeof(buf) - 1) {
        int r = httpd_req_recv(req, buf + offset, sizeof(buf) - 1 - offset);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;  // Retry on transient timeout
        }
        if (r <= 0) {
            break;     // End of body or connection closed
        }
        offset += r;
    }
    if (offset == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, R"({"error":"empty body"})", -1);
        return ESP_OK;
    }
    buf[offset] = 0;
    int len = offset;

    ESP_LOGI(TAG, "Chat REST input: %s", buf);

    // ── Extract "text" field from JSON ──────────────────────────
    const char *text_key = "\"text\":\"";
    const char *text_start = std::strstr(buf, text_key);
    std::string user_text;
    if (text_start) {
        text_start += std::strlen(text_key);
        while (*text_start && *text_start != '"') {
            user_text += *text_start++;
        }
    }

    if (user_text.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, R"({"error":"missing text field"})", -1);
        return ESP_OK;
    }

    // ── Extract "session_id" field ─────────────────────────────
    std::string session_id;
    const char *sid_key = "\"session_id\":\"";
    const char *sid_start = std::strstr(buf, sid_key);
    if (sid_start) {
        sid_start += std::strlen(sid_key);
        while (*sid_start && *sid_start != '"') {
            session_id += *sid_start++;
        }
    }

    /* ── ECHO MODE ──────────────────────────────────────────── */
    if (IS_ECHO_MODE()) {
        // Build JSON response with the text echoed back (v2.0 — no actions)
        std::string resp = R"({"type":"chat_reply","text":")";
        for (char c : user_text) {
            if (c == '"' || c == '\\') resp += '\\';
            resp += c;
        }
        resp += R"(","commands":[],"session_id":")" + session_id + R"("})";

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp.c_str(), resp.size());
        return ESP_OK;
    }

    /* ── CLOUD MODE: encrypt and forward ───────────────────── */
    size_t frame_len = 0;
    uint8_t *frame = build_upstream_frame(buf, static_cast<size_t>(len), frame_len);

    if (!frame) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, R"({"type":"error","text":"Encryption failed"})", -1);
        return ESP_OK;
    }

    // Wait for upstream connection (up to 15s — covers reconnect window)
    // Access s_upstream_connected / s_ws_sock under the mutex to avoid
    // a data race with upstream_task which writes them under the same lock.
    bool sent = false;
    for (int retry = 0; retry < 15; ++retry) {
        xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
        bool connected = s_upstream_connected && (s_ws_sock >= 0);
        xSemaphoreGive(s_sock_mutex);

        if (connected) {
            sent = send_upstream_frame(frame, frame_len);
            if (sent) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    std::free(frame);

    if (!sent) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            R"({"type":"error","text":"Upstream not available"})", -1);
        return ESP_OK;
    }

    // ── Drain stale items from previous requests ────────
    // Heap buffer to avoid stack overflow (HTTP server task has 8 KB stack).
    {
        auto *drain = static_cast<char *>(std::malloc(REPLY_TEXT_MAX));
        if (drain) {
            while (s_reply_queue && xQueueReceive(s_reply_queue, drain, 0) == pdTRUE) {}
            std::free(drain);
        }
    }

    // Wait for the downstream reply.
    // The cloud sometimes sends a preliminary empty chat_reply and then the
    // real one a few seconds later.  process_downstream_frame skips empty
    // texts, so we may need to wait through one or more empty frames.
    // We use a loop with periodic upstream-health checks to avoid hanging
    // for the full timeout if the cloud connection drops.
    auto *reply_buf = static_cast<char *>(std::malloc(REPLY_TEXT_MAX));
    if (!reply_buf) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, R"({"type":"error","text":"OOM"})", -1);
        return ESP_OK;
    }
    std::memset(reply_buf, 0, REPLY_TEXT_MAX);
    bool got_reply = false;
    TickType_t wait_start = xTaskGetTickCount();
    const TickType_t MAX_REPLY_WAIT = pdMS_TO_TICKS(30000);

    while (s_reply_queue && !got_reply) {
        TickType_t elapsed = xTaskGetTickCount() - wait_start;
        if (elapsed >= MAX_REPLY_WAIT) break;

        // Poll in 5 s chunks so we can also check for upstream drops.
        TickType_t chunk = MAX_REPLY_WAIT - elapsed;
        if (chunk > pdMS_TO_TICKS(5000)) chunk = pdMS_TO_TICKS(5000);

        if (xQueueReceive(s_reply_queue, reply_buf, chunk) == pdTRUE) {
            if (reply_buf[0] != 0) {
                got_reply = true;
                break;
            }
            // Empty text from a preliminary frame — keep waiting.
        }

        // If the upstream dropped while we were waiting there is no point
        // continuing — the reply will never arrive.
        xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
        bool connected = s_upstream_connected;
        xSemaphoreGive(s_sock_mutex);
        if (!connected) break;
    }

    if (got_reply) {
        // Return the chat reply to the PWA (v2.0 — no actions)
        std::string resp = R"({"type":"chat_reply","text":")";
        for (char *p = reply_buf; *p; ++p) {
            if (*p == '"' || *p == '\\') resp += '\\';
            resp += *p;
        }
        resp += R"(","commands":[],"session_id":")" + session_id + R"("})";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp.c_str(), resp.size());
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, R"({"type":"ack","status":"sent"})", -1);
    }

    std::free(reply_buf);
    return ESP_OK;
}

/* ── WebSocket handler for /v1/chat/ui (v2.0 — streaming) ──── */

esp_err_t chat_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Chat WebSocket connected — registering client");

        // Enable TCP keepalive on the PWA WebSocket socket so dead
        // connections are detected promptly (10 s idle, 3 s interval,
        // 3 probes = ~19 s to detect a drop).
        int fd = httpd_req_to_sockfd(req);
        if (fd >= 0) {
            int keepalive = 1;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
            int keep_idle  = 10;
            int keep_intvl = 3;
            int keep_cnt   = 3;
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &keep_idle,  sizeof(keep_idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &keep_cnt,   sizeof(keep_cnt));
        }

        // Register the PWA client for streaming push
        PwaWsClient *client = get_pwa_client(req);
        if (!client) {
            ESP_LOGW(TAG, "No free PWA client slot — rejecting");
            return ESP_FAIL;
        }
        // Reset fragmentation state for the new connection
        client->fragmenting = false;
        client->frag_len = 0;
        return ESP_OK;
    }

    // ── Handle WebSocket frame ─────────────────────────────────
    // ESP-IDF requires a two-step receive: first with len=0 to parse
    // the frame header (get type + actual payload length), then a
    // second call with the real buffer to read the payload.
    httpd_ws_frame_t ws_pkt{};
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Chat WebSocket recv header failed: %s",
                 esp_err_to_name(ret));
        release_pwa_client(req);
        return ret;
    }

    // Clamp oversized frames
    uint8_t buf[2048];
    size_t buf_size = std::min(ws_pkt.len, sizeof(buf) - 1);
    ws_pkt.payload = buf;
    ws_pkt.len = buf_size;

    ret = httpd_ws_recv_frame(req, &ws_pkt, buf_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS payload recv failed: %s", esp_err_to_name(ret));
        release_pwa_client(req);
        return ret;
    }
    buf[buf_size] = 0;  // null-terminate

    // Store the client pointer by looking up the req handle
    PwaWsClient *client = nullptr;
    if (s_pwa_mutex) {
        xSemaphoreTake(s_pwa_mutex, portMAX_DELAY);
        for (size_t i = 0; i < MAX_PWA_WS_CLIENTS; ++i) {
            if (s_pwa_clients[i].req == req) {
                client = &s_pwa_clients[i];
                break;
            }
        }
        xSemaphoreGive(s_pwa_mutex);
    }

    // ── Handle WebSocket fragmentation ─────────────────────────
    // Per RFC 6455 §5.4, a fragmented message is a sequence of:
    //   TEXT/BINARY (FIN=0) → CONTINUATION* (FIN=0) → CONTINUATION (FIN=1)
    // We reassemble fragments using the per-client frag_buf.

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && !ws_pkt.final) {
        // ── First fragment of a fragmented message ───────────
        if (!client) return ESP_OK;
        size_t copy = std::min(ws_pkt.len, sizeof(client->frag_buf) - 1);
        std::memcpy(client->frag_buf, buf, copy);
        client->frag_len = copy;
        client->frag_buf[copy] = 0;
        client->fragmenting = true;
        ESP_LOGD(TAG, "Fragmented msg start (%zu bytes)", client->frag_len);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CONTINUE) {
        if (!client || !client->fragmenting) {
            // Unexpected continuation — discard to keep stream in sync.
            // This can happen when the keepalive PING triggers a PONG from
            // the browser, and the HTTP server's internal control-frame
            // handler calls the user handler as a side effect.  Harmless.
            ESP_LOGD(TAG, "Spurious CONTINUATION frame, discarding");
            return ESP_OK;
        }
        size_t room = sizeof(client->frag_buf) - 1 - client->frag_len;
        size_t copy = std::min(ws_pkt.len, room);
        std::memcpy(client->frag_buf + client->frag_len, buf, copy);
        client->frag_len += copy;
        client->frag_buf[client->frag_len] = 0;

        if (!ws_pkt.final) {
            // More fragments to come
            ESP_LOGD(TAG, "Fragmented msg continue (%zu bytes)", client->frag_len);
            return ESP_OK;
        }

        // ── Final fragment — process complete message ─────────
        ESP_LOGD(TAG, "Fragmented msg complete (%zu bytes)", client->frag_len);
        ws_pkt.final = true;
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.len = client->frag_len;
        // Point buf and buf_size at the reassembled message
        buf_size = client->frag_len;
        std::memcpy(buf, client->frag_buf, buf_size + 1);
        client->fragmenting = false;
        client->frag_len = 0;
        // Fall through to TEXT handling below
    }

    // ── Handle text frames (unfragmented or fully reassembled) ─
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        if (ws_pkt.len < sizeof(buf)) {
            buf[ws_pkt.len] = 0;
        }

        const char *input = reinterpret_cast<const char *>(buf);
        ESP_LOGI(TAG, "Chat WS input: %.*s", (int)ws_pkt.len, input);

        // Extract session_id from the JSON and store on the client
        const char *sid_key = "\"session_id\":\"";
        const char *sid_val = std::strstr(input, sid_key);
        if (sid_val && client) {
            sid_val += std::strlen(sid_key);
            int s_idx = 0;
            while (*sid_val && *sid_val != '"' && s_idx < 63) {
                client->session_id[s_idx++] = *sid_val++;
            }
            client->session_id[s_idx] = 0;
        }

        // ── Handle session_reset ─────────────────────────────
        if (std::strstr(input, "\"session_reset\"")) {
            ESP_LOGI(TAG, "Session reset from PWA (session=%s)",
                     client ? client->session_id : "?");
            if (!IS_ECHO_MODE()) {
                // Forward upstream
                size_t frame_len = 0;
                uint8_t *frame = build_upstream_frame(
                    input, ws_pkt.len, frame_len);
                if (frame) {
                    send_upstream_frame(frame, frame_len);
                    std::free(frame);
                }
            }
            // Send ack to PWA
            const char *ack = R"({"type":"ack","status":"session_reset"})";
            httpd_ws_frame_t ack_pkt{};
            ack_pkt.payload = const_cast<uint8_t *>(
                reinterpret_cast<const uint8_t *>(ack));
            ack_pkt.len = std::strlen(ack);
            ack_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &ack_pkt);
            return ESP_OK;
        }

        // ── Handle permission_response ──────────────────────
        if (std::strstr(input, "\"permission_response\"")) {
            // Extract action_id and approved flag
            const char *aid_key = "\"action_id\":\"";
            const char *aid_val = std::strstr(input, aid_key);
            const char *app_key = "\"approved\":";
            const char *app_val = std::strstr(input, app_key);

            if (aid_val && app_val && s_perm_mutex && s_perm_semaphore) {
                aid_val += std::strlen(aid_key);
                char resp_id[64];
                int idx = 0;
                while (*aid_val && *aid_val != '"' && idx < 63) {
                    resp_id[idx++] = *aid_val++;
                }
                resp_id[idx] = 0;

                // Parse approved: true/false
                app_val += std::strlen(app_key);
                bool approved = (std::strncmp(app_val, "true", 4) == 0);

                ESP_LOGI(TAG, "Permission response: action_id=%s approved=%d",
                         resp_id, approved);

                // Check if this matches the pending request
                xSemaphoreTake(s_perm_mutex, portMAX_DELAY);
                if (std::strcmp(s_perm_action_id, resp_id) == 0) {
                    s_perm_approved = approved;
                    // Signal the waiting Lua thread
                    xSemaphoreGive(s_perm_semaphore);
                    ESP_LOGI(TAG, "  → Signalled permission semaphore");
                } else {
                    ESP_LOGW(TAG, "  → Action ID mismatch: pending='%s' got='%s'",
                             s_perm_action_id, resp_id);
                }
                xSemaphoreGive(s_perm_mutex);
            }

            // Send acknowledgment to PWA
            const char *ack = R"({"type":"ack","status":"permission_received"})";
            httpd_ws_frame_t ack_pkt{};
            ack_pkt.payload = const_cast<uint8_t *>(
                reinterpret_cast<const uint8_t *>(ack));
            ack_pkt.len = std::strlen(ack);
            ack_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &ack_pkt);
            return ESP_OK;
        }

        /* ── ECHO MODE ───────────────────────────────────────── */
        if (IS_ECHO_MODE()) {
            std::string echo_resp = R"({"type":"chat_reply","text":")" +
                std::string(input, ws_pkt.len) +
                R"(","commands":[],"session_id":")" +
                (client ? client->session_id : "") + R"("})";

            httpd_ws_frame_t resp_pkt{};
            resp_pkt.payload = reinterpret_cast<uint8_t *>(echo_resp.data());
            resp_pkt.len = echo_resp.size();
            resp_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &resp_pkt);
            return ESP_OK;
        }

        /* ── CLOUD MODE: encrypt and forward ──────────────────── */
        size_t frame_len = 0;
        uint8_t *frame = build_upstream_frame(input, ws_pkt.len, frame_len);

        if (!frame) {
            const char *err = R"({"type":"error","text":"Encryption failed"})";
            httpd_ws_frame_t err_pkt{};
            err_pkt.payload = const_cast<uint8_t *>(
                reinterpret_cast<const uint8_t *>(err));
            err_pkt.len = std::strlen(err);
            err_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &err_pkt);
            return ESP_OK;
        }

        // Send upstream — non-blocking, don't wait for reply
        // (reply comes via broadcast_to_pwa from the upstream task)
        bool sent = false;
        {
            xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
            bool connected = s_upstream_connected && (s_ws_sock >= 0);
            if (connected) {
                sent = send_upstream_frame(frame, frame_len);
            }
            xSemaphoreGive(s_sock_mutex);
        }
        std::free(frame);

        if (sent) {
            const char *ack = R"({"type":"ack","status":"sent"})";
            httpd_ws_frame_t ack_pkt{};
            ack_pkt.payload = const_cast<uint8_t *>(
                reinterpret_cast<const uint8_t *>(ack));
            ack_pkt.len = std::strlen(ack);
            ack_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &ack_pkt);
        } else {
            const char *err = R"({"type":"error","text":"Upstream not available"})";
            httpd_ws_frame_t err_pkt{};
            err_pkt.payload = const_cast<uint8_t *>(
                reinterpret_cast<const uint8_t *>(err));
            err_pkt.len = std::strlen(err);
            err_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &err_pkt);
        }

    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Chat WebSocket closed by client");
        if (client) {
            client->fragmenting = false;
            client->frag_len = 0;
        }
        release_pwa_client(req);
    } else {
        ESP_LOGW(TAG, "Unexpected WS frame type 0x%02x", ws_pkt.type);
    }

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
 *  OpenClaw Action Telemetry & Permission API
 * ═══════════════════════════════════════════════════════════════ */

void broadcast_action(const char *session_id,
                      const char *type,
                      const char *description,
                      const char *status,
                      const char *action_id)
{
    if (!type || !description || !status) return;

    // Build the JSON action message
    char json[1024];
    std::string esc_type = json_escape(type);
    std::string esc_desc = json_escape(description);
    std::string esc_stat = json_escape(status);
    std::string esc_sid  = session_id ? json_escape(session_id) : "";
    std::string esc_aid  = action_id  ? json_escape(action_id)  : "";

    int n;
    if (!esc_aid.empty()) {
        n = std::snprintf(json, sizeof(json),
            R"({"type":"openclaw_action","action_type":"%s","description":"%s")"
            R"(,"status":"%s","action_id":"%s","session_id":"%s"})",
            esc_type.c_str(), esc_desc.c_str(),
            esc_stat.c_str(), esc_aid.c_str(), esc_sid.c_str());
    } else {
        n = std::snprintf(json, sizeof(json),
            R"({"type":"openclaw_action","action_type":"%s","description":"%s")"
            R"(,"status":"%s","session_id":"%s"})",
            esc_type.c_str(), esc_desc.c_str(),
            esc_stat.c_str(), esc_sid.c_str());
    }

    if (n > 0) {
        broadcast_to_pwa(session_id, json);
        ESP_LOGI(TAG, "Action telemetry: %s [%s] → %s",
                 type, status, description);
    }
}

void report_action(const char *type,
                   const char *description,
                   const char *status)
{
    broadcast_action(nullptr, type, description, status, "");
}

bool request_permission(const char *type,
                        const char *description,
                        uint32_t timeout_ms)
{
    if (!type || !description) return false;
    if (!s_perm_semaphore || !s_perm_mutex) return false;

    // Generate a unique action ID using a timestamp
    char action_id[64];
    std::snprintf(action_id, sizeof(action_id), "perm_%lld_%d",
                  (long long)esp_timer_get_time(), rand());

    // Store the pending action ID under the mutex
    xSemaphoreTake(s_perm_mutex, portMAX_DELAY);
    std::strncpy(s_perm_action_id, action_id, sizeof(s_perm_action_id) - 1);
    s_perm_action_id[sizeof(s_perm_action_id) - 1] = '\0';
    s_perm_approved = false;
    // Reset the semaphore before broadcasting
    xSemaphoreTake(s_perm_semaphore, 0);
    xSemaphoreGive(s_perm_mutex);

    // Broadcast the permission request to all PWA clients
    broadcast_action(nullptr, type, description, "pending", action_id);

    ESP_LOGI(TAG, "Permission requested: %s (%s) — waiting %ums",
             type, description, timeout_ms);

    // Wait for the user's response (limited by timeout)
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    bool signalled = (xSemaphoreTake(s_perm_semaphore, ticks) == pdTRUE);

    // Read the result
    xSemaphoreTake(s_perm_mutex, portMAX_DELAY);
    bool approved = signalled ? s_perm_approved : false;
    std::strncpy(action_id, s_perm_action_id, sizeof(action_id) - 1);
    action_id[sizeof(action_id) - 1] = '\0';
    // Clear the pending state
    s_perm_action_id[0] = '\0';
    xSemaphoreGive(s_perm_mutex);

    // Broadcast the result
    broadcast_action(nullptr, type, description,
                     approved ? "approved" : (signalled ? "denied" : "timeout"),
                     action_id);

    ESP_LOGI(TAG, "Permission result for '%s': %s (signalled=%d approved=%d)",
             type, approved ? "APPROVED" : "DENIED/TIMEOUT", signalled, approved);

    return approved;
}

}  // namespace network
