#include "network/http_server.h"
#include "network/chat_ws.h"
#include "network/marketplace_proxy.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"
#include "network/www_assets.h"
#include "util/log_ring.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cerrno>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "fs/littlefs_manager.h"
#include "lua/lua_vm.h"
#include "robot/driver_board.h"
#include "sdkconfig.h"
#include "robot/robot.h"
#include "wasm/wasm_sandbox.h"

#include "lwip/sockets.h"
#ifndef LWIP_SOCKET_OFFSET
#define LWIP_SOCKET_OFFSET 0
#endif

static const char *TAG = "http_server";

namespace network {
namespace {

httpd_handle_t s_server = nullptr;

/* ── MIME type helpers ──────────────────────────────────────── */

const char *get_mime_type(const char *path)
{
    const char *ext = std::strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (std::strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (std::strcmp(ext, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (std::strcmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (std::strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (std::strcmp(ext, ".json") == 0) return "application/json";
    if (std::strcmp(ext, ".png")  == 0) return "image/png";
    if (std::strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (std::strcmp(ext, ".wasm") == 0) return "application/wasm";
    if (std::strcmp(ext, ".mpxe") == 0) return "application/wasm";
    return "application/octet-stream";
}

/* ── URL-decode a percent-encoded string ───────────────────── */
static std::string url_decode(const std::string &src)
{
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            char *end = nullptr;
            long val = std::strtol(hex, &end, 16);
            if (end == hex + 2) {
                out += static_cast<char>(val);
                i += 2;
            } else {
                out += src[i];
            }
        } else if (src[i] == '+') {
            out += ' ';
        } else {
            out += src[i];
        }
    }
    return out;
}

/* ── Map a request URI to an embedded asset name ─────────────── */
std::string uri_to_asset(const char *uri)
{
    // Default to index.html for root
    if (std::strcmp(uri, "/") == 0) {
        return "/index.html";
    }

    // Servo Studio lives at a bare /studio
    if (std::strcmp(uri, "/studio") == 0) {
        return "/studio.html";
    }

    // Map favicon.ico to the design-appropriate SVG icon
    if (std::strcmp(uri, "/favicon.ico") == 0) {
#ifdef CONFIG_PWA_DESIGN_REDESIGN
        return "/md.svg";
#else
        return "/icon.svg";
#endif
    }

    std::string path(uri);

    // req->uri carries the query string too — "/m.js?v=3" must still resolve.
    const std::size_t q = path.find('?');
    if (q != std::string::npos) {
        path.resize(q);
    }

    // Directory request → its index
    if (!path.empty() && path.back() == '/') {
        path += "index.html";
    }

    return path;
}

/* ── Check if client accepts gzip ───────────────────────────── */
bool accepts_gzip(httpd_req_t *req)
{
    char buf[64] = {};
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    return std::strstr(buf, "gzip") != nullptr;
}

/* ── ETag for an embedded asset ──────────────────────────────
 *
 * The last 8 bytes of a gzip stream are [CRC32 of the uncompressed data]
 * followed by [uncompressed size], both little-endian. That CRC32 is already a
 * strong content hash, so we get a correct ETag for free — no hashing of the
 * payload at request time.
 */
static void make_etag(const uint8_t *data, std::size_t len,
                      char *out, std::size_t out_len)
{
    uint32_t crc = 0;
    if (len >= 8) {
        std::memcpy(&crc, data + len - 8, sizeof(crc));
    }
    std::snprintf(out, out_len, "\"%08x-%x\"",
                  static_cast<unsigned>(crc), static_cast<unsigned>(len));
}

/* ── Serve an asset out of the memory-mapped firmware image ────
 *
 * Assets are embedded pre-gzipped only (see main/CMakeLists.txt), so `data`
 * points straight into mapped flash: nothing is read into RAM, no file handle
 * is opened, and no LittleFS cache is allocated. lwip's own TCP send window is
 * the only buffer involved.
 */
static bool send_embedded(httpd_req_t *req, const char *name)
{
    std::size_t len = 0;
    const uint8_t *data = embedded_asset(name, &len);
    if (!data || len == 0) {
        return false;
    }

    char etag[32];
    make_etag(data, len, etag, sizeof(etag));

    // Revalidate rather than cache-forever: asset names are not content-hashed,
    // so a firmware update must be able to invalidate them. The 304 path costs
    // one small response instead of re-sending the whole payload.
    char inm[64] = {};
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        std::strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, "", 0) == ESP_OK;
    }

    if (httpd_resp_set_type(req, get_mime_type(name)) != ESP_OK) return false;
    if (httpd_resp_set_hdr(req, "Content-Encoding", "gzip") != ESP_OK) return false;
    if (httpd_resp_set_hdr(req, "Cache-Control", "no-cache") != ESP_OK) return false;
    if (httpd_resp_set_hdr(req, "ETag", etag) != ESP_OK) return false;

    if (!accepts_gzip(req)) {
        // Every browser advertises gzip. A client that does not (raw curl,
        // a minimal HTTP library) gets the compressed bytes anyway, because
        // there is no uncompressed copy on the device any more.
        ESP_LOGW(TAG, "Client did not advertise gzip for %s - sending compressed", name);
    }

    return httpd_resp_send(req, reinterpret_cast<const char *>(data),
                           static_cast<ssize_t>(len)) == ESP_OK;
}

/* ── Static asset handler ───────────────────────────────────── */
esp_err_t static_handler(httpd_req_t *req)
{
    // Reject API/WebSocket paths
    if (std::strncmp(req->uri, "/v1/", 4) == 0) {
        return ESP_OK;
    }

    const std::string name = uri_to_asset(req->uri);

    if (send_embedded(req, name.c_str())) {
        return ESP_OK;
    }

    // Unknown path → index.html, so client-side SPA routes resolve.
    if (send_embedded(req, "/index.html")) {
        return ESP_OK;
    }

    // Nothing found — send a minimal manual response and return OK
    // (returning ESP_FAIL triggers a cleanup crash in ESP-IDF's httpd)
    ESP_LOGW(TAG, "No embedded asset for %s, sending 404", req->uri);
    const char *body = "404 Not Found";
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── WebSocket telemetry handler ────────────────────────────── */
esp_err_t websocket_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket connection established");
        return ESP_OK;
    }

    // Handle WebSocket frame
    httpd_ws_frame_t ws_pkt{};
    uint8_t buf[256] = {};

    ws_pkt.payload = buf;
    ws_pkt.len = sizeof(buf);

    const esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket recv error: %s", esp_err_to_name(ret));
        return ret;
    }

    // Echo or handle the message
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGD(TAG, "WS text: %.*s", (int)ws_pkt.len, (const char *)ws_pkt.payload);
        // Echo back for now
        httpd_ws_send_frame(req, &ws_pkt);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket closed by client");
    }

    return ESP_OK;
}

}  // anonymous namespace

/* ── REST API handlers ─────────────────────────────────────── */

/* ── Helper: check if a path is under /www/ (read-only PWA assets) ── */
static bool is_www_path(const std::string &path)
{
    return path.compare(0, 5, "/www/") == 0 || path == "/www";
}

/* GET /v1/skills/list — list .wasm files (excluding www) */
static esp_err_t api_skills_list(httpd_req_t *req)
{
    auto files = fs::list_files("/");
    std::string json = "[\n";

    bool first = true;
    for (const auto &f : files) {
        // Accept .wasm and .mpxe skill files
        bool is_skill = (f.size() >= 6 && f.substr(f.size() - 5) == ".wasm")
                     || (f.size() >= 6 && f.substr(f.size() - 5) == ".mpxe");
        if (!is_skill) continue;
        if (is_www_path(f)) continue;

        if (!first) json += ",\n";
        first = false;

        std::size_t sz = fs::file_size(f.c_str());
        std::string name = f;
        if (name.size() > 0 && name[0] == '/') name = name.substr(1);

        json += "  {\"name\":\"" + name + "\",\"size\":" + std::to_string(sz) + "}";
    }
    json += "\n]";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* ── Async skill execution ───────────────────────────────────────
 * WASM skills can run for up to 60 s.  Running them inline in the HTTP
 * handler blocked the whole web server for that entire time — connections
 * piled up and the socket pool was exhausted ("accept (23)", stuck after a
 * dance).  Instead we launch the skill on its own FreeRTOS task and return
 * immediately, so httpd stays responsive while the skill runs.
 */
static volatile bool s_skill_running = false;

/* ── Last-run bookkeeping ─────────────────────────────────────────
 * Because the run is async, POST /v1/skills/run can only ever answer
 * "started" — the outcome arrives up to 60 s later and used to exist
 * nowhere but the serial log. The web UI showed an alert saying "started"
 * whether the skill worked or trapped on its first instruction.
 *
 * These few variables are what GET /v1/skills/status reports, so the UI can
 * show a real running indicator and then the actual result. Only the skill
 * task writes them and only httpd reads them, one skill at a time, so no
 * lock is needed — but the reader must not see a half-updated name, hence
 * the name is assigned before `running` flips and never while it is true.
 */
/* Defined further down with the filesystem handlers; the status endpoint
 * below needs it first. */
static std::string json_escape(const std::string &s);

static std::string s_skill_current;        /* name of the running skill */
static std::string s_skill_last_name;      /* name of the previous run  */
static int         s_skill_last_result = -1;   /* SandboxResult, -1 = none yet */
static int64_t     s_skill_started_us  = 0;
static int64_t     s_skill_last_ms     = 0;    /* how long the last run took */

/* Human-readable form of wasm::SandboxResult, for the status endpoint. */
static const char *skill_result_text(int r)
{
    switch (static_cast<wasm::SandboxResult>(r)) {
        case wasm::SandboxResult::Success:           return "ok";
        case wasm::SandboxResult::LoadFailed:        return "load failed — not a valid .wasm";
        case wasm::SandboxResult::InstantiateFailed: return "instantiate failed — check imports";
        case wasm::SandboxResult::FunctionNotFound:  return "no on_start export";
        case wasm::SandboxResult::ExecutionFailed:   return "trapped during execution";
        case wasm::SandboxResult::Timeout:           return "timed out (60 s)";
        case wasm::SandboxResult::NotInitialized:    return "sandbox not initialised";
        default:                                     return "unknown";
    }
}

struct SkillRunArgs { std::string path; std::string name; };

static void skill_run_task(void *arg)
{
    SkillRunArgs *a = static_cast<SkillRunArgs *>(arg);
    ESP_LOGI(TAG, "Running skill (async): %s", a->path.c_str());
    auto result = wasm::load_and_run(a->path.c_str(), "on_start", 60000);
    ESP_LOGI(TAG, "Skill '%s' completed with result=%d",
             a->name.c_str(), static_cast<int>(result));

    /* Publish the outcome before clearing `running`, so a poll that sees
     * running=false is guaranteed to already see the matching result. */
    s_skill_last_name   = a->name;
    s_skill_last_result = static_cast<int>(result);
    s_skill_last_ms     = (esp_timer_get_time() - s_skill_started_us) / 1000;
    s_skill_current.clear();

    delete a;
    s_skill_running = false;
    vTaskDelete(nullptr);
}

/* ── Installed-skill manifest ─────────────────────────────────────
 *
 * Which marketplace skill owns which file on this robot used to be recorded
 * ONLY in the browser's localStorage, and recovered by regex-scraping the Lua
 * deploy script for file_write("...") calls. That had three consequences, all
 * of which happened: install from your phone and your laptop offered to
 * install again; uninstall from a second device deleted nothing because its
 * localStorage was empty, while the UI reported success; and clearing site
 * data orphaned every installed file permanently, with no way to find them.
 *
 * The robot owns its own filesystem, so the robot keeps the record. Flat
 * JSON, parsed with the same strstr helpers as the rest of this file — no
 * parser to add, and hand-editable if it ever needs rescuing.
 */
/* Defined further down, next to the handlers they belong with. The installed
 * manifest has to sit above api_skills_upload (which records provenance into
 * it) but below nothing in particular, so it lands above these three. */
static char *read_body(httpd_req_t *req);
static std::string json_get_str(const char *body, const char *key);
static bool skill_filename_ok(const std::string &name);

static constexpr const char *INSTALLED_PATH = "/installed.json";

static std::string installed_read()
{
    auto bytes = fs::read_file(INSTALLED_PATH);
    if (bytes.empty()) return "{\"skills\":[]}";
    return std::string(bytes.begin(), bytes.end());
}

/* Rewrites the manifest with `skill_id` removed, then optionally re-added.
 * A whole-file rewrite rather than an in-place edit: the file is a few hundred
 * bytes, LittleFS has no atomic rename here, and correctness beats cleverness
 * at this size. */
static bool installed_write(const std::string &skill_id,
                            const std::string &file,
                            const std::string &version,
                            const std::string &title,
                            bool remove_only)
{
    const std::string current = installed_read();
    std::string out = "{\"skills\":[";
    bool first = true;

    // Copy every entry except the one we are replacing or removing.
    std::size_t pos = 0;
    const std::string key = "{\"skill_id\":\"";
    while ((pos = current.find(key, pos)) != std::string::npos) {
        const std::size_t start = pos;
        const std::size_t end = current.find('}', start);
        if (end == std::string::npos) break;
        const std::string entry = current.substr(start, end - start + 1);
        pos = end + 1;

        const std::size_t id_start = key.size();
        const std::size_t id_end = entry.find('"', id_start);
        if (id_end == std::string::npos) continue;
        if (entry.substr(id_start, id_end - id_start) == skill_id) continue;

        if (!first) out += ",";
        out += entry;
        first = false;
    }

    if (!remove_only) {
        if (!first) out += ",";
        out += "{\"skill_id\":\"" + json_escape(skill_id) + "\"";
        out += ",\"file\":\"" + json_escape(file) + "\"";
        out += ",\"version\":\"" + json_escape(version) + "\"";
        out += ",\"title\":\"" + json_escape(title) + "\"}";
    }

    out += "]}";
    return fs::write_file(INSTALLED_PATH, out.data(), out.size());
}

/* GET /v1/skills/installed — what is on this robot and where it came from. */
static esp_err_t api_skills_installed(httpd_req_t *req)
{
    const std::string body = installed_read();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

/* POST /v1/skills/uninstall {"skill_id":"..."} — delete the file AND the entry.
 *
 * Doing both is the point. The old flow could remove an entitlement without
 * removing the file (leaving a refunded skill runnable) or remove files
 * without clearing the record (leaving a phantom install).
 */
static esp_err_t api_skills_uninstall(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"empty or oversized body\"}", -1);
        return ESP_OK;
    }
    const std::string skill_id = json_get_str(body, "skill_id");
    free(body);

    if (skill_id.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing skill_id\"}", -1);
        return ESP_OK;
    }

    // Find the file this skill installed, so uninstall is not guesswork.
    const std::string current = installed_read();
    const std::string needle = "{\"skill_id\":\"" + skill_id + "\"";
    std::string file;
    const std::size_t at = current.find(needle);
    if (at != std::string::npos) {
        const std::size_t fk = current.find("\"file\":\"", at);
        if (fk != std::string::npos) {
            const std::size_t fs_ = fk + 8;
            const std::size_t fe = current.find('"', fs_);
            if (fe != std::string::npos) file = current.substr(fs_, fe - fs_);
        }
    }

    bool deleted = false;
    if (!file.empty() && skill_filename_ok(file)) {
        deleted = fs::delete_file(("/" + file).c_str());
    }
    installed_write(skill_id, "", "", "", /*remove_only=*/true);

    std::string resp = "{\"ok\":true,\"file\":\"" + json_escape(file)
                     + "\",\"deleted\":" + (deleted ? "true" : "false") + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* GET /v1/logs?since=<seq>&max=<n> — the robot's own log, over HTTP.
 *
 * Polled rather than streamed. httpd runs with max_open_sockets = 5 and
 * lru_purge_enable, so a permanently-held log socket would be a fifth of the
 * budget and a candidate for eviction — it could get itself, or the chat
 * socket, dropped. Polling costs nothing between requests.
 *
 * `since` is the sequence number from the previous response's "next", so a
 * client never re-reads a line. Omit it (or pass 0) for everything held.
 */
static esp_err_t api_logs(httpd_req_t *req)
{
    uint32_t since = 0;
    std::size_t max_lines = 200;

    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
        }
        if (httpd_query_key_value(query, "max", val, sizeof(val)) == ESP_OK) {
            const long m = std::strtol(val, nullptr, 10);
            // Bounded because the whole response is built in RAM.
            if (m > 0 && m < 500) max_lines = static_cast<std::size_t>(m);
        }
    }

    uint32_t next = since;
    std::string lines = util::log_ring_json(since, max_lines, next);

    std::string resp = "{\"next\":" + std::to_string(next)
                     + ",\"lines\":" + lines + "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* GET /v1/skills/status — is a skill running, and how did the last one go? */
static esp_err_t api_skills_status(httpd_req_t *req)
{
    bool running = s_skill_running;

    std::string json = "{\"running\":";
    json += running ? "true" : "false";

    if (running) {
        json += ",\"name\":\"" + json_escape(s_skill_current) + "\"";
        json += ",\"elapsed_ms\":" +
                std::to_string((esp_timer_get_time() - s_skill_started_us) / 1000);
    }

    if (s_skill_last_result >= 0) {
        json += ",\"last\":{\"name\":\"" + json_escape(s_skill_last_name) + "\"";
        json += ",\"result\":" + std::to_string(s_skill_last_result);
        json += ",\"ok\":";
        json += (s_skill_last_result ==
                 static_cast<int>(wasm::SandboxResult::Success)) ? "true" : "false";
        json += ",\"message\":\"" +
                json_escape(skill_result_text(s_skill_last_result)) + "\"";
        json += ",\"duration_ms\":" + std::to_string(s_skill_last_ms) + "}";
    }

    json += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* POST /v1/skills/run — execute a .wasm skill via WAMR (async) */
static esp_err_t api_skills_run(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const char *key = "\"skill\":\"";
    const char *val = std::strstr(buf, key);
    if (!val) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing skill name", -1);
        return ESP_OK;
    }
    val += std::strlen(key);
    std::string skill_name;
    while (*val && *val != '"') skill_name += *val++;

    std::string path = "/" + skill_name;

    // ── Reject if a skill is already running (single WASM instance) ──
    if (s_skill_running) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"output\":\"a skill is already running\"}", -1);
        return ESP_OK;
    }

    // ── Launch the skill on its own task and return immediately ──
    // Set the name and start time BEFORE flipping `running`, so the first
    // status poll — which can land microseconds later — never sees
    // running=true with a stale or empty name.
    s_skill_current    = skill_name;
    s_skill_started_us = esp_timer_get_time();
    s_skill_running    = true;
    auto *args = new SkillRunArgs{ path, skill_name };
    // No core affinity: the scheduler keeps httpd (core 0) responsive while the
    // skill yields during its robot_delay_ms calls.  8 KB task stack is enough;
    // the WASM operand stack is allocated separately inside load_and_run.
    BaseType_t created = xTaskCreate(skill_run_task, "skill_run", 8192,
                                     args, 4, nullptr);
    if (created != pdPASS) {
        s_skill_running = false;
        s_skill_current.clear();
        delete args;
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"output\":\"failed to start skill task\"}", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Skill '%s' started (async)", skill_name.c_str());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"output\":\"started\"}", -1);
    return ESP_OK;
}

/* Escape a string for JSON embedding. */
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

/* GET /v1/fs/list — list files + dirs on LittleFS. Query: ?path=/lua (default /) */
static esp_err_t api_fs_list(httpd_req_t *req)
{
    std::string dir_path = "/";
    const char *query = strchr(req->uri, '?');
    if (query) {
        const char *p = strstr(query, "path=");
        if (p) {
            dir_path = p + 5;
            auto amp = dir_path.find('&');
            if (amp != std::string::npos) dir_path = dir_path.substr(0, amp);
            dir_path = url_decode(dir_path);
        }
    }

    // Build VFS path, open directory, enumerate files + subdirs
    std::string vfs = (dir_path == "/") ? "/fs" : "/fs" + dir_path;
    DIR *dir = opendir(vfs.c_str());

    std::string files_json, dirs_json;
    bool first_file = true, first_dir = true;

    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == ".") continue;

            if (entry->d_type == DT_DIR) {
                if (!first_dir) dirs_json += ",";
                first_dir = false;
                dirs_json += "\"" + json_escape(name) + "\"";
            } else if (entry->d_type == DT_REG) {
                std::string full = dir_path + "/" + name;
                std::size_t sz = fs::file_size(full.c_str());
                bool ro = is_www_path(full);
                if (!first_file) files_json += ",";
                first_file = false;
                files_json += "{\"n\":\"" + json_escape(name) + "\""
                            + ",\"s\":" + std::to_string(sz)
                            + ",\"r\":" + (ro ? "true" : "false") + "}";
            }
        }
        closedir(dir);
    }

    std::string json = "{\"f\":[" + files_json + "],\"d\":[" + dirs_json + "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* GET /v1/fs/read?path=/lua/walk.lua — read file content as JSON */
static esp_err_t api_fs_read(httpd_req_t *req)
{
    std::string file_path;
    const char *query = strchr(req->uri, '?');
    if (query) {
        const char *p = strstr(query, "path=");
        if (p) {
            file_path = p + 5;
            auto amp = file_path.find('&');
            if (amp != std::string::npos) file_path = file_path.substr(0, amp);
            file_path = url_decode(file_path);
        }
    }

    if (file_path.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing path", -1);
        return ESP_OK;
    }

    auto data = fs::read_file(file_path.c_str());
    if (data.empty()) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "file not found", -1);
        return ESP_OK;
    }

    std::string content(data.begin(), data.end());
    std::string resp = "{\"ok\":true,\"p\":\"" + json_escape(file_path)
                     + "\",\"c\":\"" + json_escape(content) + "\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* GET /v1/fs/info — filesystem stats */
static esp_err_t api_fs_info(httpd_req_t *req)
{
    std::size_t total = 0, used = 0;
    fs::stats(total, used);

    char json[128];
    std::snprintf(json, sizeof(json),
                  "{\"total\":%zu,\"used\":%zu}", total, used);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

/* POST /v1/fs/delete — delete a file (reject www paths) */
static esp_err_t api_fs_delete(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const char *key = "\"path\":\"";
    const char *val = std::strstr(buf, key);
    if (!val) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing path", -1);
        return ESP_OK;
    }
    val += std::strlen(key);
    std::string file_path = "/";
    while (*val && *val != '"') file_path += *val++;

    // Check sudo mode
    bool sudo = (std::strstr(buf, "\"sudo\":true") != nullptr);

    // Reject deletion of www assets (even with sudo)
    if (is_www_path(file_path)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "cannot delete www assets", -1);
        return ESP_OK;
    }

    // Without sudo, only allow .wasm and .lua deletion
    if (!sudo) {
        std::string lower = file_path;
        for (auto &c : lower) c = std::tolower(c);
        bool allowed = (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".wasm")
                    || (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".mpxe")
                    || (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".lua");
        if (!allowed) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_send(req, "sudo required to delete this file", -1);
            return ESP_OK;
        }
    }

    if (fs::delete_file(file_path.c_str())) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "delete failed", -1);
    }
    return ESP_OK;
}

/* POST /v1/skills/upload — upload a .wasm file (raw body, name in query) */
/* Validate an uploaded skill's filename.
 *
 * This used to be a raw substring scrape with no checks at all: no
 * url_decode, no ".." rejection, no "/" rejection, no extension check, no
 * length cap — and the result went straight into fopen("/fs/" + name, "wb").
 * `?name=../../foo` was a write anywhere the VFS would follow, and
 * `?name=www/index.html.gz` would overwrite the web UI's own assets.
 *
 * A skill filename is a bare leaf name ending in .wasm or .mpxe. Nothing else
 * is legitimate, so accept nothing else.
 */
static bool skill_filename_ok(const std::string &name)
{
    if (name.empty() || name.size() > 64) return false;

    // No path structure of any kind: no separators, no traversal, no leading
    // dot (which would also cover "..", "." and hidden files).
    if (name.find('/')  != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    if (name[0] == '.') return false;

    // Conservative charset — anything outside it is far likelier to be an
    // attack or an encoding bug than a filename someone meant to type.
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9')
                     || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }

    // Must be a skill. wasm_sandbox already enforces this at run time; there
    // is no reason to let anything else onto the filesystem in the first place.
    const bool is_wasm = name.size() > 5 && name.compare(name.size() - 5, 5, ".wasm") == 0;
    const bool is_mpxe = name.size() > 5 && name.compare(name.size() - 5, 5, ".mpxe") == 0;
    return is_wasm || is_mpxe;
}

static esp_err_t api_skills_upload(httpd_req_t *req)
{
    // Extract filename from query string: /v1/skills/upload?name=foo.wasm
    std::string filename = "skill.wasm";
    const char *query = strchr(req->uri, '?');
    if (query) {
        const char *nkey = "name=";
        const char *nval = std::strstr(query + 1, nkey);
        if (nval) {
            nval += std::strlen(nkey);
            filename = "";
            while (*nval && *nval != '&') filename += *nval++;
        }
    }

    // Decode BEFORE validating, or "%2e%2e%2f" walks straight past the checks.
    filename = url_decode(filename);

    // Optional provenance: &skill_id=&version=&title=. Absent for a plain
    // `mpx-cli deploy` of your own build, present when `mpx-cli install` or the
    // PWA puts a marketplace skill here — which is what lets the robot, rather
    // than a browser's localStorage, know what it is carrying.
    std::string skill_id, version, title;
    if (query) {
        auto param = [&](const char *key) -> std::string {
            std::string needle = std::string(key) + "=";
            const char *v = std::strstr(query + 1, needle.c_str());
            if (!v) return "";
            v += needle.size();
            std::string out;
            while (*v && *v != '&') out += *v++;
            return url_decode(out);
        };
        skill_id = param("skill_id");
        version  = param("version");
        title    = param("title");
    }

    if (!skill_filename_ok(filename)) {
        ESP_LOGW(TAG, "Rejected skill upload with unsafe name: '%s'", filename.c_str());
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid filename — expected a "
                             "bare name ending in .wasm or .mpxe\"}", -1);
        return ESP_OK;
    }

    // Read the raw binary body
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 256 * 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid size", -1);
        return ESP_OK;
    }

    auto *data = new uint8_t[content_len];
    size_t total_read = 0;

    while (total_read < content_len) {
        int ret = httpd_req_recv(req, (char *)data + total_read,
                                 content_len - total_read);
        if (ret <= 0) break;
        total_read += ret;
    }

    if (total_read != content_len) {
        delete[] data;
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "read error", -1);
        return ESP_OK;
    }

    // Write to LittleFS
    std::string fs_path = "/" + filename;
    if (fs::write_file(fs_path.c_str(), data, total_read)) {
        ESP_LOGI(TAG, "Uploaded %s (%zu bytes)", filename.c_str(), total_read);
        if (!skill_id.empty()) {
            installed_write(skill_id, filename, version, title, /*remove_only=*/false);
            ESP_LOGI(TAG, "Recorded install: %s -> %s", skill_id.c_str(), filename.c_str());
        }
        std::string resp = "{\"ok\":true,\"path\":\"" + filename + "\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp.c_str(), resp.size());
    } else {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "write failed", -1);
    }

    delete[] data;
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
 *  Robot control API
 * ═══════════════════════════════════════════════════════════════ */

/* ── Helper: parse a JSON string value (handles escape sequences) ── */
static std::string json_get_str(const char *body, const char *key)
{
    std::string needle = "\"" + std::string(key) + "\":\"";
    const char *p = std::strstr(body, needle.c_str());
    if (!p) return {};
    p += needle.size();
    std::string val;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++; // skip backslash
            if (*p == '"')       val += '"';
            else if (*p == '\\') val += '\\';
            else if (*p == 'n')  val += '\n';
            else if (*p == 't')  val += '\t';
            else if (*p == 'r')  val += '\r';
            else if (*p == '/')  val += '/';
            else if (*p == 'u') {
                // Simple \\u00xx for basic ASCII range
                if (p[1] && p[2] && p[3] && p[4]) {
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    unsigned int code;
                    sscanf(hex, "%x", &code);
                    val += (char)code;
                    p += 4;
                }
            } else {
                val += *p; // pass through unknown escape
            }
        } else {
            val += *p;
        }
        p++;
    }
    return val;
}

/* ── Helper: parse a JSON number value ─────────────────────── */
static int json_get_int(const char *body, const char *key, int def)
{
    std::string needle = "\"" + std::string(key) + "\":";
    const char *p = std::strstr(body, needle.c_str());
    if (!p) return def;
    p += needle.size();
    // Skip whitespace
    while (*p == ' ') ++p;
    bool neg = (*p == '-');
    if (neg) ++p;
    int val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); ++p; }
    return neg ? -val : val;
}

/* ── Helper: parse a JSON float value ──────────────────────── */
static float json_get_float(const char *body, const char *key, float def)
{
    std::string needle = "\"" + std::string(key) + "\":";
    const char *p = std::strstr(body, needle.c_str());
    if (!p) return def;
    p += needle.size();
    while (*p == ' ') ++p;
    char *end = nullptr;
    float val = std::strtof(p, &end);
    return (end == p) ? def : val;
}

/* ── Gait name → GaitCmd mapping ──────────────────────────── */
static robot::GaitCmd gait_name_to_cmd(const std::string &name)
{
    if (name == "init")     return robot::GaitCmd::Init;
    if (name == "step")     return robot::GaitCmd::Step;
    if (name == "roll")     return robot::GaitCmd::Roll;
    if (name == "pitch")    return robot::GaitCmd::Pitch;
    if (name == "stretch")  return robot::GaitCmd::Stretch;
    if (name == "advance")  return robot::GaitCmd::Advance;
    if (name == "back")     return robot::GaitCmd::Back;
    if (name == "left")     return robot::GaitCmd::Left;
    if (name == "right")    return robot::GaitCmd::Right;
    if (name == "turnL")    return robot::GaitCmd::TurnL;
    if (name == "turnR")    return robot::GaitCmd::TurnR;
    if (name == "twerk")    return robot::GaitCmd::Twerk;
    if (name == "jump")     return robot::GaitCmd::Jump;
    if (name == "jumpfwd")  return robot::GaitCmd::JumpFwd;
    if (name == "testspeed") return robot::GaitCmd::TestSpeed;
    if (name == "lookup")    return robot::GaitCmd::LookUp;
    if (name == "lookdown")  return robot::GaitCmd::LookDown;
    if (name == "lookleft")  return robot::GaitCmd::LookLeft;
    if (name == "lookright") return robot::GaitCmd::LookRight;
    if (name == "lookul")    return robot::GaitCmd::LookUpperLeft;
    if (name == "lookur")    return robot::GaitCmd::LookUpperRight;
    if (name == "lookll")    return robot::GaitCmd::LookLowerLeft;
    if (name == "looklr")    return robot::GaitCmd::LookLowerRight;
    if (name == "flegL")     return robot::GaitCmd::ForelegLiftL;
    if (name == "flegR")     return robot::GaitCmd::ForelegLiftR;
    if (name == "blegL")     return robot::GaitCmd::BacklegLiftL;
    if (name == "blegR")     return robot::GaitCmd::BacklegLiftR;
    if (name == "heightup")  return robot::GaitCmd::HeightUp;
    if (name == "heightdown")return robot::GaitCmd::HeightDown;
    if (name == "balance")   return robot::GaitCmd::Balance;
    if (name == "bowback")   return robot::GaitCmd::BowBack;
    if (name == "bodycycle") return robot::GaitCmd::BodyCycle;
    if (name == "headellipse")return robot::GaitCmd::HeadEllipse;
    if (name == "moveLF")    return robot::GaitCmd::MoveLeftFront;
    if (name == "moveRF")    return robot::GaitCmd::MoveRightFront;
    if (name == "moveLB")    return robot::GaitCmd::MoveLeftBack;
    if (name == "moveRB")    return robot::GaitCmd::MoveRightBack;
    if (name == "stanford")  return robot::GaitCmd::StanfordWalk;
    if (name == "frontkick") return robot::GaitCmd::FrontKick;
    if (name == "wiggle")    return robot::GaitCmd::Wiggle;
    if (name == "buttshrug") return robot::GaitCmd::ButtShrug;
    if (name == "wiggleL")   return robot::GaitCmd::WiggleLeft;
    if (name == "wiggleR")   return robot::GaitCmd::WiggleRight;
    if (name == "buttshrugL")return robot::GaitCmd::ButtShrugLeft;
    if (name == "buttshrugR")return robot::GaitCmd::ButtShrugRight;
    if (name == "none")     return robot::GaitCmd::None;
    return robot::GaitCmd::None;
}

/* ── GaitCmd → name string ─────────────────────────────────── */
static const char *gait_cmd_to_name(robot::GaitCmd cmd)
{
    switch (cmd) {
        case robot::GaitCmd::None:      return "none";
        case robot::GaitCmd::Init:      return "init";
        case robot::GaitCmd::Step:      return "step";
        case robot::GaitCmd::Roll:      return "roll";
        case robot::GaitCmd::Pitch:     return "pitch";
        case robot::GaitCmd::Stretch:   return "stretch";
        case robot::GaitCmd::Advance:   return "advance";
        case robot::GaitCmd::Back:      return "back";
        case robot::GaitCmd::Left:      return "left";
        case robot::GaitCmd::Right:     return "right";
        case robot::GaitCmd::TurnL:     return "turnL";
        case robot::GaitCmd::TurnR:     return "turnR";
        case robot::GaitCmd::Twerk:     return "twerk";
        case robot::GaitCmd::Jump:      return "jump";
        case robot::GaitCmd::JumpFwd:   return "jumpfwd";
        case robot::GaitCmd::TestSpeed: return "testspeed";
        case robot::GaitCmd::LookUp:         return "lookup";
        case robot::GaitCmd::LookDown:       return "lookdown";
        case robot::GaitCmd::LookLeft:       return "lookleft";
        case robot::GaitCmd::LookRight:      return "lookright";
        case robot::GaitCmd::LookUpperLeft:  return "lookul";
        case robot::GaitCmd::LookUpperRight: return "lookur";
        case robot::GaitCmd::LookLowerLeft:  return "lookll";
        case robot::GaitCmd::LookLowerRight: return "looklr";
        case robot::GaitCmd::ForelegLiftL:   return "flegL";
        case robot::GaitCmd::ForelegLiftR:   return "flegR";
        case robot::GaitCmd::BacklegLiftL:   return "blegL";
        case robot::GaitCmd::BacklegLiftR:   return "blegR";
        case robot::GaitCmd::HeightUp:       return "heightup";
        case robot::GaitCmd::HeightDown:     return "heightdown";
        case robot::GaitCmd::Balance:        return "balance";
        case robot::GaitCmd::BowBack:        return "bowback";
        case robot::GaitCmd::BodyCycle:      return "bodycycle";
        case robot::GaitCmd::HeadEllipse:    return "headellipse";
        case robot::GaitCmd::MoveLeftFront:  return "moveLF";
        case robot::GaitCmd::MoveRightFront: return "moveRF";
        case robot::GaitCmd::MoveLeftBack:   return "moveLB";
        case robot::GaitCmd::MoveRightBack:  return "moveRB";
        case robot::GaitCmd::StanfordWalk:   return "stanford";
        case robot::GaitCmd::FrontKick:      return "frontkick";
        case robot::GaitCmd::Wiggle:         return "wiggle";
        case robot::GaitCmd::ButtShrug:      return "buttshrug";
        case robot::GaitCmd::WiggleLeft:     return "wiggleL";
        case robot::GaitCmd::WiggleRight:    return "wiggleR";
        case robot::GaitCmd::ButtShrugLeft:  return "buttshrugL";
        case robot::GaitCmd::ButtShrugRight: return "buttshrugR";
        case robot::GaitCmd::BodyAttitude:   return "attitude";
    }
    return "unknown";
}

/* POST /v1/robot/gait — set gait mode */
static esp_err_t api_robot_gait(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    std::string mode = json_get_str(buf, "mode");
    if (mode.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'mode' field", -1);
        return ESP_OK;
    }

    robot::GaitCmd cmd = gait_name_to_cmd(mode);

    ESP_LOGI(TAG, "POST /v1/robot/gait  mode=%s", mode.c_str());

    robot::send_gait_cmd(cmd);

    char resp[128];
    std::snprintf(resp, sizeof(resp), R"({"ok":true,"mode":"%s"})", gait_cmd_to_name(cmd));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/joy — web joystick input (mini_pupper_web_controller
 * style).  Body: {"f":-1..1,"s":-1..1,"t":-1..1}
 *   f = forward/back, s = strafe (+left), t = turn (+left).
 * Touching a pad auto-starts the Stanford walk; releasing (zeros)
 * steps in place.  Velocities scale with Config::sg_speed.        */
static esp_err_t api_robot_joy(httpd_req_t *req)
{
    char buf[160] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const float f = json_get_float(buf, "f", 0.0f);
    const float s = json_get_float(buf, "s", 0.0f);
    const float t = json_get_float(buf, "t", 0.0f);

    robot::joy_input(f, s, t);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* GET /v1/robot/status — current gait mode */
static esp_err_t api_robot_status(httpd_req_t *req)
{
    robot::GaitCmd cmd = robot::current_gait_cmd();
    robot::Config cfg = robot::get_config();

    ESP_LOGI(TAG, "GET  /v1/robot/status  mode=%s", gait_cmd_to_name(cmd));

    char resp[512];
    int n = std::snprintf(resp, sizeof(resp),
        R"({"mode":"%s")"
        R"(,"config":{"period":%d,"height":%d,"up_height":%d,"stride":%d,"tilt":%d,"sg_speed":%d})"
        R"(,"offsets":[)",
        gait_cmd_to_name(cmd),
        cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt, cfg.sg_speed);

    for (int i = 1; i <= 12; ++i) {
        n += std::snprintf(resp + n, sizeof(resp) - n, "%.1f%s",
                           robot::get_offset(i), (i < 12) ? "," : "");
    }
    n += std::snprintf(resp + n, sizeof(resp) - n, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/config — update robot configuration */
static esp_err_t api_robot_config(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    robot::Config cfg = robot::get_config();

    int val;
    if ((val = json_get_int(buf, "period", -1))   >= 0)   cfg.period    = val;
    if ((val = json_get_int(buf, "height", -1))   >= 0)   cfg.height    = val;
    if ((val = json_get_int(buf, "up_height", -1)) >= 0)   cfg.up_height = val;
    if ((val = json_get_int(buf, "stride", -1))   >= 0)   cfg.stride    = val;
    if ((val = json_get_int(buf, "tilt", -1))     >= 0)   cfg.tilt      = val;
    if ((val = json_get_int(buf, "sg_speed", -1)) >= 0)   cfg.sg_speed  = val;

    ESP_LOGI(TAG, "POST /v1/robot/config  period=%d height=%d up_height=%d stride=%d tilt=%d sg_speed=%d",
             cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt, cfg.sg_speed);

    robot::set_config(cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* GET /v1/robot/config — get robot configuration */
static esp_err_t api_robot_get_config(httpd_req_t *req)
{
    robot::Config cfg = robot::get_config();

    ESP_LOGI(TAG, "GET  /v1/robot/config  period=%d height=%d up_height=%d stride=%d tilt=%d",
             cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt);

    char resp[256];
    std::snprintf(resp, sizeof(resp),
        R"({"period":%d,"height":%d,"up_height":%d,"stride":%d,"tilt":%d,"sg_speed":%d})",
        cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt, cfg.sg_speed);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/calibrate — set a servo offset */
static esp_err_t api_robot_calibrate(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    int servo = json_get_int(buf, "servo", -1);
    float offset = json_get_float(buf, "offset", 0.0f);

    if (servo < 1 || servo > 12) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "servo must be 1-12", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "POST /v1/robot/calibrate  servo=%d offset=%.1f", servo, offset);

    robot::set_offset(servo, offset);

    char resp[128];
    std::snprintf(resp, sizeof(resp),
                  R"({"ok":true,"servo":%d,"offset":%.1f})", servo, offset);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/calibrate/reset — reset all offsets to 0 */
static esp_err_t api_robot_cal_reset(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /v1/robot/calibrate/reset");
    robot::reset_offsets();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/robot/diagnostic/ping — ping a servo by ID */
static esp_err_t api_robot_diag_ping(httpd_req_t *req)
{
    char buf[64] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    int servo = json_get_int(buf, "servo", 1);
    if (servo < 1 || servo > 12) servo = 1;

    ESP_LOGI(TAG, "POST /v1/robot/diagnostic/ping  servo=%d", servo);

    int result = robot::ping_servo(servo);
    char resp[128];
    if (result > 0) {
        std::snprintf(resp, sizeof(resp),
                      R"({"ok":true,"servo":%d,"model":%d})", servo, result);
    } else {
        std::snprintf(resp, sizeof(resp),
                      R"({"ok":false,"servo":%d,"error":%d})", servo, result);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/diagnostic/sweep — sweep a single servo ±45° */
static esp_err_t api_robot_diag_sweep(httpd_req_t *req)
{
    char buf[64] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    int servo = json_get_int(buf, "servo", 1);
    if (servo < 1 || servo > 12) servo = 1;

    ESP_LOGI(TAG, "POST /v1/robot/diagnostic/sweep  servo=%d", servo);

    // Sweep servo back and forth
    for (int i = 0; i < 3; ++i) {
        robot::set_servo_angle(servo, -45.0f);
        robot::set_all_servo_speed(200);
        robot::flush();
        vTaskDelay(pdMS_TO_TICKS(400));

        robot::set_servo_angle(servo, 45.0f);
        robot::flush();
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    // Return to centre
    robot::set_servo_angle(servo, 0.0f);
    robot::flush();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
 *  WiFi config API
 * ═══════════════════════════════════════════════════════════════ */

/* GET /v1/wifi/status — current AP + STA status */
static esp_err_t api_wifi_status(httpd_req_t *req)
{
    StaState sta = wifi_sta_get_state();
    std::string sta_ip = wifi_sta_get_ip();
    std::string sta_ssid = wifi_sta_get_ssid();

    const char *sta_state_str = "disconnected";
    switch (sta) {
        case StaState::Disconnected: sta_state_str = "disconnected"; break;
        case StaState::Connecting:   sta_state_str = "connecting";   break;
        case StaState::Connected:    sta_state_str = "connected";    break;
        case StaState::Failed:       sta_state_str = "failed";       break;
    }

    char resp[512];
    std::snprintf(resp, sizeof(resp),
        R"({"ap":{"ssid":"%s","ip":"%s"})"
        R"(,"sta":{"state":"%s","ssid":"%s","ip":"%s"}})",
        wifi_ap_get_ssid(), AP_IP_ADDR,
        sta_state_str, sta_ssid.c_str(), sta_ip.c_str());

    ESP_LOGI(TAG, "GET  /v1/wifi/status  ap_ssid=%s sta=%s ssid=%s ip=%s",
             wifi_ap_get_ssid(), sta_state_str, sta_ssid.c_str(), sta_ip.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/wifi/ap-config — save AP SSID/password and restart.
 *
 * Body: {"ssid":"MyAP","password":"secret123"}
 * Responds 200 before restarting so the client gets a clean response.
 * The ESP will reboot ~500 ms after the response is sent, giving the
 * PWA time to display the reconnect instructions.
 */
static esp_err_t api_wifi_ap_config(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    std::string ssid     = json_get_str(buf, "ssid");
    std::string password = json_get_str(buf, "password");

    if (ssid.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'ssid' field", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "POST /v1/wifi/ap-config  ssid=%s  pass_len=%zu",
             ssid.c_str(), password.size());

    if (!wifi_ap_save_config(ssid.c_str(), password.c_str())) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "nvs write failed", -1);
        return ESP_OK;
    }

    // Respond to the client before restarting
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true,"restarting":true})", -1);

    ESP_LOGW(TAG, "AP config saved — restarting ESP in 500 ms ...");

    // Flush logs, then restart
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; // never reached
}

/* POST /v1/wifi/connect — connect to a Wi-Fi network (STA mode) */
static esp_err_t api_wifi_connect(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    std::string ssid = json_get_str(buf, "ssid");
    std::string password = json_get_str(buf, "password");

    if (ssid.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'ssid' field", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "POST /v1/wifi/connect  ssid=%s", ssid.c_str());

    if (!wifi_sta_connect(ssid.c_str(), password.c_str())) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "connect failed", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/wifi/disconnect — disconnect from STA */
static esp_err_t api_wifi_disconnect(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /v1/wifi/disconnect");
    wifi_sta_disconnect();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/wifi/forget — forget saved credentials and disconnect */
static esp_err_t api_wifi_forget(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /v1/wifi/forget");
    wifi_sta_forget();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* ── Body reader helper (heap-allocated, no large stack buffers) ──────── */

/**
 * Read the full request body into a heap-allocated buffer.
 * Caller must free() the returned pointer. Returns NULL on failure.
 */
// Largest request body any handler will buffer. Same as the skill upload cap.
static constexpr int MAX_REQUEST_BODY = 256 * 1024;

static char *read_body(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0) return nullptr;

    // Bounded. This used to malloc whatever Content-Length claimed, so a
    // single request could ask for hundreds of KB of contiguous ESP32 heap and
    // drain it out from under the WASM sandbox and the TLS buffers. The cap
    // matches the skill upload limit, which is the largest legitimate body any
    // of these handlers receives.
    if (content_len > MAX_REQUEST_BODY) {
        ESP_LOGW(TAG, "Request body too large: %d bytes (max %d)",
                 content_len, MAX_REQUEST_BODY);
        return nullptr;
    }

    char *buf = (char *)malloc(content_len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory reading a %d byte body", content_len);
        return nullptr;
    }

    int total = 0;
    while (total < content_len) {
        int ret = httpd_req_recv(req, buf + total, content_len - total);
        if (ret <= 0) {
            free(buf);
            return nullptr;
        }
        total += ret;
    }
    buf[total] = '\0';
    return buf;
}

/* ── Lua API endpoints ─────────────────────────────────────── */

/* POST /v1/lua/enqueue — enqueue a Lua script for async execution.
 * Saves the script to a temp file and queues the path to the worker.
 * Non-blocking — returns immediately.  Script runs in lua_worker_task. */
static esp_err_t api_lua_enqueue(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string script = json_get_str(buf, "script");
    free(buf);

    if (script.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "need 'script' field", -1);
        return ESP_OK;
    }

    // Write script to temp file on LittleFS
    static int enqueue_seq = 0;
    char path[64];
    std::snprintf(path, sizeof(path), "/lua/_deploy_%d.lua", enqueue_seq++);
    if (!fs::write_file(path, script.data(), script.size())) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, R"({"ok":false,"error":"Failed to save script"})", -1);
        return ESP_OK;
    }

    // Enqueue the file path — worker reads and executes via lua_run_file
    if (!network::queue_lua_script(path)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, R"({"ok":false,"error":"Lua queue full"})", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/lua/run — run a Lua script
 *   Body: {"script":"..."}  or  {"path":"/lua/foo.lua"}
 *   Response: {"ok":true,"output":"..."}  or  {"ok":false,"error":"..."}
 */
static esp_err_t api_lua_run(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string script = json_get_str(buf, "script");
    std::string path   = json_get_str(buf, "path");
    free(buf);

    if (script.empty() && path.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "need 'script' or 'path' field", -1);
        return ESP_OK;
    }

    // Use a heap-allocated output buffer (2048 bytes is enough for most results)
    char *output = (char *)malloc(2048);
    if (!output) {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_send(req, "oom", -1);
        return ESP_OK;
    }
    output[0] = '\0';

    esp_err_t err;
    if (!path.empty()) {
        err = lua_run_file(path.c_str(), output, 2048, 5000);
    } else {
        err = lua_run_string(script.c_str(), output, 2048, 5000);
    }

    std::string escaped = json_escape(output);
    free(output);

    std::string resp = (err == ESP_OK)
        ? R"({"ok":true,"output":")" + escaped + "\"}"
        : R"({"ok":false,"error":")" + escaped + "\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* GET /v1/lua/list — list .lua files on LittleFS
 *   Response: {"ok":true,"files":["walk.lua","test.lua"]}
 */
static esp_err_t api_lua_list(httpd_req_t *req)
{
    auto files = fs::list_files("/lua");
    std::string json = R"({"ok":true,"files":[)";
    for (size_t i = 0; i < files.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + json_escape(files[i]) + "\"";
    }
    json += "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* POST /v1/lua/save — save a Lua script to LittleFS
 *   Body: {"name":"walk.lua","code":"print('hi')"}
 */
static esp_err_t api_lua_save(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string name = json_get_str(buf, "name");
    std::string code = json_get_str(buf, "code");
    free(buf);

    if (name.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'name' field", -1);
        return ESP_OK;
    }

    std::string path = "/lua/" + name;
    bool ok = fs::write_file(path.c_str(), code.data(), code.size());

    char resp[256];
    std::snprintf(resp, sizeof(resp),
                  R"({"ok":%s,"path":"%s"})", ok ? "true" : "false",
                  json_escape(path).c_str());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* GET /v1/lua/read?name=walk.lua — read a Lua script */
static esp_err_t api_lua_read(httpd_req_t *req)
{
    std::string name;
    std::string query(req->uri);
    auto pos = query.find("name=");
    if (pos != std::string::npos) {
        name = query.substr(pos + 5);
        auto amp = name.find('&');
        if (amp != std::string::npos) name = name.substr(0, amp);
        name = url_decode(name);
    }

    if (name.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'name' query param", -1);
        return ESP_OK;
    }

    std::string path = "/lua/" + name;
    auto data = fs::read_file(path.c_str());

    if (data.empty()) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "file not found", -1);
        return ESP_OK;
    }

    std::string code(data.begin(), data.end());
    std::string resp = R"({"ok":true,"name":")" + json_escape(name)
                     + "\",\"code\":\"" + json_escape(code) + "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* POST /v1/lua/delete — delete a Lua script
 *   Body: {"name":"walk.lua"}
 */
static esp_err_t api_lua_delete(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string name = json_get_str(buf, "name");
    free(buf);

    if (name.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'name' field", -1);
        return ESP_OK;
    }

    std::string path = "/lua/" + name;
    bool ok = fs::delete_file(path.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? R"({"ok":true})" : R"({"ok":false})", -1);
    return ESP_OK;
}


/* ═══════════════════════════════════════════════════════════════
 *  MangDang Servo Studio API   (/v1/studio/...)
 *
 *  Thin JSON wrapper over robot/driver_board.h, ported from the /api/... routes
 *  of the mpxesp test firmware. Two rules carried over from there, both of
 *  which are about who owns the SPI bus:
 *
 *   - Every PARAMETER operation requires studio mode. A config exchange is a
 *     request/reply pair; gait traffic in between loses the reply and leaves a
 *     config frame pending that the next feedback decode misreads.
 *   - Read-only telemetry (status, temps) deliberately does NOT require it —
 *     the AT32 puts position, current and NTC temperature in every feedback
 *     frame, so while the gait runs those are already fresh and free.
 * ═══════════════════════════════════════════════════════════════ */

static int studio_qint(httpd_req_t *r, const char *key, int def)
{
    char q[128], v[24];
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, key, v, sizeof v) == ESP_OK) {
        return atoi(v);
    }
    return def;
}

static float studio_qfloat(httpd_req_t *r, const char *key, float def)
{
    char q[128], v[24];
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, key, v, sizeof v) == ESP_OK) {
        return strtof(v, nullptr);
    }
    return def;
}

static esp_err_t studio_json(httpd_req_t *r, const char *s)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
}

#define STUDIO_NEED_MODE(r)                                                   \
    do {                                                                      \
        if (!robot::studio_mode())                                            \
            return studio_json((r), R"({"ok":false,"err":"studio mode off"})"); \
    } while (0)

#define STUDIO_NEED_ID(r, id)                                                 \
    do {                                                                      \
        if ((id) < 1 || (id) > 12)                                            \
            return studio_json((r), R"({"ok":false,"err":"bad id"})");        \
    } while (0)

/* GET /v1/studio/mode[?on=0|1] — read or set studio mode. */
static esp_err_t studio_mode_handler(httpd_req_t *req)
{
    const int on = studio_qint(req, "on", -1);
    if (on == 0 || on == 1) {
        robot::set_studio_mode(on == 1);
    }
    char b[64];
    std::snprintf(b, sizeof b, R"({"ok":true,"studio":%s})",
                  robot::studio_mode() ? "true" : "false");
    return studio_json(req, b);
}

/* GET /v1/studio/status — all 12 servos: position, current, temperature.
 * Works with the gait running; that is the point of it. */
static esp_err_t studio_status_handler(httpd_req_t *req)
{
    // Parked gait means a stale cache — replay each board's last frame so the
    // numbers on screen are live. This is safe: an idle servo stays idle.
    if (robot::studio_mode()) {
        for (int bd = 0; bd < 4; ++bd) driver_board_poll_board(bd);
    }

    char b[768];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"studio":%s,"servos":[)",
                          robot::studio_mode() ? "true" : "false");
    for (int id = 1; id <= 12; ++id) {
        const uint16_t scs = driver_board_present_position(id);
        const float    deg = static_cast<float>(scs) * 270.0f / 1024.0f;
        const float    t   = driver_board_present_temperature(id);
        n += std::snprintf(b + n, sizeof b - n,
                           R"(%s{"id":%d,"scs":%u,"deg":%.1f,"ma":%d,"c":)",
                           id == 1 ? "" : ",", id, scs, deg,
                           driver_board_present_current(id));
        if (t > DB_TEMP_INVALID) n += std::snprintf(b + n, sizeof b - n, "%.1f}", t);
        else                     n += std::snprintf(b + n, sizeof b - n, "null}");
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/scan — which channels answer a parameter read. */
static esp_err_t studio_scan_handler(httpd_req_t *req)
{
    STUDIO_NEED_MODE(req);
    char b[128];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"found":[)");
    bool first = true;
    for (int id = 1; id <= 12; ++id) {
        float v;
        if (driver_board_get_param(id, DB_PARAM_KP_POSITION, &v)) {
            n += std::snprintf(b + n, sizeof b - n, "%s%d", first ? "" : ",", id);
            first = false;
        }
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/dump?id=N — every config parameter of one servo. */
static esp_err_t studio_dump_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);

    char b[640];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"id":%d,"params":[)", id);
    for (int p = 0; p < DB_PARAM_COUNT; ++p) {
        float v;
        const char *name = driver_board_param_name(p);
        if (driver_board_get_param(id, p, &v)) {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"p":%d,"n":"%s","v":%g})",
                               p ? "," : "", p, name ? name : "", v);
        } else {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"p":%d,"n":"%s","v":null})",
                               p ? "," : "", p, name ? name : "");
        }
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/set?id=N&p=P&v=V — write one parameter to RAM, read it back. */
static esp_err_t studio_set_handler(httpd_req_t *req)
{
    const int   id = studio_qint(req, "id", 0);
    const int   p  = studio_qint(req, "p", -1);
    const float v  = studio_qfloat(req, "v", 0);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);
    if (p < 0 || p >= DB_PARAM_COUNT) {
        return studio_json(req, R"({"ok":false,"err":"bad param"})");
    }

    float rb = 0;
    if (driver_board_set_param(id, p, v) && driver_board_get_param(id, p, &rb)) {
        char b[96];
        std::snprintf(b, sizeof b, R"({"ok":true,"p":%d,"v":%g})", p, rb);
        return studio_json(req, b);
    }
    return studio_json(req, R"({"ok":false,"err":"no reply"})");
}

/* GET /v1/studio/setall?p=P&v=V[&save=1] — one parameter to all 12 servos. */
static esp_err_t studio_setall_handler(httpd_req_t *req)
{
    const int   p    = studio_qint(req, "p", -1);
    const int   save = studio_qint(req, "save", 0);
    const float v    = studio_qfloat(req, "v", 0);
    STUDIO_NEED_MODE(req);
    if (p < 0 || p >= DB_PARAM_COUNT) {
        return studio_json(req, R"({"ok":false,"err":"bad param"})");
    }

    char fail[64];
    int fn = 0, n = 0;
    fail[0] = '\0';
    for (int id = 1; id <= 12; ++id) {
        float rb = 0;
        if (driver_board_set_param(id, p, v) && driver_board_get_param(id, p, &rb)) {
            n++;
        } else {
            fn += std::snprintf(fail + fn, sizeof fail - fn, "%s%d", fn ? "," : "", id);
        }
        vTaskDelay(pdMS_TO_TICKS(2));   // give the AT32 time between writes
    }
    const bool saved = (save && n) ? driver_board_save_config(-1) : false;

    char b[192];
    std::snprintf(b, sizeof b,
                  R"({"ok":%s,"p":%d,"v":%g,"n":%d,"fail":[%s],"saved":%s})",
                  n ? "true" : "false", p, v, n, fail, saved ? "true" : "false");
    return studio_json(req, b);
}

/* GET /v1/studio/live?id=N — the AT32's live control-loop values. */
static esp_err_t studio_live_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);

    float lv[DB_LIVE_COUNT];
    bool ok = true;
    // Everything below DB_LIVE_TEMPERATURE_C is required; temperature is
    // optional so a board on pre-NTC AT32 firmware degrades to "no temp"
    // instead of dropping the whole trace to the reduced path.
    for (int i = 0; i < DB_LIVE_TEMPERATURE_C && ok; ++i) {
        ok = driver_board_get_live(id, i, &lv[i]);
    }

    char b[640];
    if (ok) {
        float t;
        bool have_t = driver_board_get_live(id, DB_LIVE_TEMPERATURE_C, &t);
        if (!have_t || t <= DB_TEMP_INVALID) {
            t = driver_board_present_temperature(id);
            have_t = (t > DB_TEMP_INVALID);
        }
        int n = std::snprintf(b, sizeof b,
            R"({"ok":true,"full":true,"pos_adc":%g,"cur_adc":%g,)"
            R"("set_deg":%g,"now_deg":%g,"err_deg":%g,)"
            R"("cap_ma":%g,"set_ma":%g,"now_ma":%g,"err_ma":%g,)"
            R"("duty":%g,"mode":%d,"loop":%lu)",
            lv[DB_LIVE_POS_ADC], lv[DB_LIVE_CUR_ADC],
            lv[DB_LIVE_SETPOINT_POS_DEG], lv[DB_LIVE_PRESENT_POS_DEG],
            lv[DB_LIVE_ERROR_POS_DEG],
            lv[DB_LIVE_MAX_CURRENT_MA], lv[DB_LIVE_SETPOINT_CUR_MA],
            lv[DB_LIVE_PRESENT_CUR_MA], lv[DB_LIVE_ERROR_CUR_MA],
            lv[DB_LIVE_PWM_DUTY], static_cast<int>(lv[DB_LIVE_MODE]),
            static_cast<unsigned long>(lv[DB_LIVE_LOOP_COUNTER]));
        if (have_t) n += std::snprintf(b + n, sizeof b - n, R"(,"temp_c":%.1f)", t);
        std::snprintf(b + n, sizeof b - n, "}");
    } else if (driver_board_poll(id)) {
        const float t = driver_board_present_temperature(id);
        int n = std::snprintf(b, sizeof b,
            R"({"ok":true,"full":false,"now_deg":%g,"now_ma":%d)",
            static_cast<float>(driver_board_present_position(id)) * 270.0f / 1024.0f,
            driver_board_present_current(id));
        if (t > DB_TEMP_INVALID) n += std::snprintf(b + n, sizeof b - n, R"(,"temp_c":%.1f)", t);
        std::snprintf(b + n, sizeof b - n, "}");
    } else {
        std::snprintf(b, sizeof b, R"({"ok":false,"err":"spi"})");
    }
    return studio_json(req, b);
}

/* GET /v1/studio/temps — NTC temperature of all 12. No studio mode needed. */
static esp_err_t studio_temps_handler(httpd_req_t *req)
{
    if (robot::studio_mode()) {
        for (int bd = 0; bd < 4; ++bd) driver_board_poll_board(bd);
    }
    char b[512];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"studio":%s,"temps":[)",
                          robot::studio_mode() ? "true" : "false");
    for (int id = 1; id <= 12; ++id) {
        const float t = driver_board_present_temperature(id);
        if (t > DB_TEMP_INVALID) {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"id":%d,"c":%.1f})",
                               id == 1 ? "" : ",", id, t);
        } else {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"id":%d,"c":null})",
                               id == 1 ? "" : ",", id);
        }
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/save?id=N — commit board config to AT32 flash (0 = all). */
static esp_err_t studio_save_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    const int board = (id >= 1 && id <= 12) ? (id - 1) / 3 : -1;
    return studio_json(req, driver_board_save_config(board)
                              ? R"({"ok":true})"
                              : R"({"ok":false,"err":"save failed"})");
}

/* GET /v1/studio/restore?id=N — factory defaults, RAM only (0 = all). */
static esp_err_t studio_restore_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    const int board = (id >= 1 && id <= 12) ? (id - 1) / 3 : -1;
    return studio_json(req, driver_board_factory_restore(board)
                              ? R"({"ok":true})"
                              : R"({"ok":false,"err":"restore failed"})");
}

/* GET /v1/studio/direct?id=N&m=0|1|2&deg=..&cur=..
 * m: 0 idle (motor off), 1 position hold, 2 torque. deg is the RAW AT32 angle
 * 0..270 with 135 = centre — the same units as the AT32's own CLI. */
static esp_err_t studio_direct_handler(httpd_req_t *req)
{
    const int   id  = studio_qint(req, "id", 0);
    const int   m   = studio_qint(req, "m", 0);
    const float deg = studio_qfloat(req, "deg", 135.0f);
    const float cur = studio_qfloat(req, "cur", 130.0f);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);

    const uint16_t mode = (m == 1) ? DB_MODE_POSITION
                        : (m == 2) ? DB_MODE_TORQUE
                                   : DB_MODE_IDLE;
    return studio_json(req, driver_board_direct(id, mode, deg,
                                                static_cast<int16_t>(cur))
                              ? R"({"ok":true})"
                              : R"({"ok":false,"err":"spi"})");
}

/* ── Helper to register an API handler ─────────────────────── */
static void register_api(httpd_handle_t server, const char *method_str,
                         const char *uri, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t h = {
        .uri       = uri,
        .method    = method,
        .handler   = handler,
        .user_ctx  = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &h);
    ESP_LOGI(TAG, "  API: %s %s", method_str, uri);
}

/* ═══════════════════════════════════════════════════════════════
 *  Marketplace Gateway Proxy API
 * ═══════════════════════════════════════════════════════════════ */

/* GET /v1/gateway/config — return gateway connection info */
static esp_err_t api_gateway_config(httpd_req_t *req)
{
    std::string host = CONFIG_APP_CHAT_SERVER_IP;
    int port = CONFIG_APP_CHAT_SERVER_PORT;
    std::string uuid = CONFIG_APP_ROBOT_UUID;

    char resp[512];
    int n = std::snprintf(resp, sizeof(resp),
        R"({"host":"%s","port":%d,"robot_uuid":"%s"})",
        host.c_str(), port, uuid.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}

/**
 * @brief Generic proxy handler for all /v1/marketplace/ endpoints.
 *
 * Translates the local URI into the corresponding Gateway path and
 * forwards the request via gateway_request().
 *
 * URI translation rules:
 *   /v1/marketplace/skills[/...]        -> /v1/skills[/...]
 *   /v1/marketplace/robot/skills[/...]  -> /v1/robots/{uuid}/skills[/...]
 */
/* ── Marketplace GET response cache ──────────────────────────────
 * Serving repeated marketplace polls from a short-lived cache avoids one
 * outbound proxy socket PER request.  Under a stress test the browser hammers
 * these read-only endpoints, and each proxied request cost an inbound + an
 * outbound socket — doubling demand until the LWIP pool ran dry.  Caching GETs
 * for a few seconds collapses that back to (at most) one proxy call per TTL.
 */
struct MpxCacheEntry {
    std::string  path;
    std::string  body;
    TickType_t   expires = 0;
    bool         valid = false;
};
static MpxCacheEntry     s_mpx_cache[4];
static SemaphoreHandle_t s_mpx_cache_mutex = nullptr;
static constexpr uint32_t MPX_CACHE_TTL_MS = 15000;   // 15 s

static SemaphoreHandle_t mpx_cache_mutex()
{
    if (!s_mpx_cache_mutex) s_mpx_cache_mutex = xSemaphoreCreateMutex();
    return s_mpx_cache_mutex;
}

// Return cached body for `path` if present and unexpired, else empty string.
static std::string mpx_cache_get(const std::string &path)
{
    SemaphoreHandle_t m = mpx_cache_mutex();
    if (!m) return {};
    std::string out;
    xSemaphoreTake(m, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    for (auto &e : s_mpx_cache) {
        if (e.valid && e.path == path && (int32_t)(e.expires - now) > 0) {
            out = e.body;
            break;
        }
    }
    xSemaphoreGive(m);
    return out;
}

static void mpx_cache_put(const std::string &path, const std::string &body)
{
    SemaphoreHandle_t m = mpx_cache_mutex();
    if (!m) return;
    xSemaphoreTake(m, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    // Reuse a matching/empty/expired slot, else the oldest.
    MpxCacheEntry *slot = nullptr;
    for (auto &e : s_mpx_cache) {
        if (!e.valid || e.path == path || (int32_t)(e.expires - now) <= 0) { slot = &e; break; }
    }
    if (!slot) slot = &s_mpx_cache[0];
    slot->path    = path;
    slot->body    = body;
    slot->expires = now + pdMS_TO_TICKS(MPX_CACHE_TTL_MS);
    slot->valid   = true;
    xSemaphoreGive(m);
}

// Drop all cached entries (called after any write — deploy/remove/etc.).
static void mpx_cache_invalidate_all()
{
    SemaphoreHandle_t m = mpx_cache_mutex();
    if (!m) return;
    xSemaphoreTake(m, portMAX_DELAY);
    for (auto &e : s_mpx_cache) e.valid = false;
    xSemaphoreGive(m);
}

static esp_err_t api_marketplace_proxy(httpd_req_t *req)
{
    // ── Determine the target Gateway path ─────────────────────
    const char *uri = req->uri;
    std::string target_path;

    const char *prefix_skills = "/v1/marketplace/skills";
    const char *prefix_robot  = "/v1/marketplace/robot/";

    if (std::strncmp(uri, prefix_robot, std::strlen(prefix_robot)) == 0) {
        // /v1/marketplace/robot/{action}[/...] → /v1/robots/{uuid}/{action}[/...]
        // e.g.  /v1/marketplace/robot/skills      → /v1/robots/{uuid}/skills
        //       /v1/marketplace/robot/skills/{id}  → /v1/robots/{uuid}/skills/{id}
        //       /v1/marketplace/robot/deploy       → /v1/robots/{uuid}/deploy
        std::string suffix = uri + std::strlen(prefix_robot);
        target_path = "/v1/robots/" + std::string(CONFIG_APP_ROBOT_UUID) + "/" + suffix;
    } else if (std::strncmp(uri, prefix_skills, std::strlen(prefix_skills)) == 0) {
        // /v1/marketplace/skills[/...] → /v1/skills[/...]
        // Strip "/v1/marketplace/" (16 chars), prepend "/v1/"
        target_path = std::string("/v1/") + (uri + 16);
    } else {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "unknown marketplace path", -1);
        return ESP_OK;
    }

    // ── Read request body if present ──────────────────────────
    std::string body;
    // Previously an over-size body was silently DROPPED and the request
    // forwarded with no body at all, so the gateway rejected it and the user
    // saw an upstream error for a request that never left intact.
    if (req->content_len >= 4096) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"request body too large for the proxy\"}", -1);
        return ESP_OK;
    }
    if (req->content_len > 0) {
        char *buf = static_cast<char *>(std::malloc(req->content_len + 1));
        if (buf) {
            int total = 0;
            while (total < req->content_len) {
                int r = httpd_req_recv(req, buf + total, req->content_len - total);
                if (r <= 0) break;
                total += r;
            }
            buf[total] = 0;
            body.assign(buf, total);
            std::free(buf);
        }
    }

    // ── Map HTTP method ───────────────────────────────────────
    const char *method_str = "GET";
    if (req->method == HTTP_POST)   method_str = "POST";
    else if (req->method == HTTP_PATCH)  method_str = "PATCH";
    else if (req->method == HTTP_DELETE) method_str = "DELETE";

    const bool is_get = (req->method == HTTP_GET);

    // ── Serve GETs from the short-lived cache (no proxy socket) ──
    if (is_get) {
        std::string cached = mpx_cache_get(target_path);
        if (!cached.empty()) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, cached.c_str(), cached.size());
            return ESP_OK;
        }
    } else {
        // Any write changes the catalog — drop cached reads.
        mpx_cache_invalidate_all();
    }

    // ── Forward to Gateway ────────────────────────────────────
    bool ok = false;
    int upstream_status = 0;
    std::string resp_body = gateway_request(method_str, target_path.c_str(),
                                            body, ok, &upstream_status);

    ESP_LOGI(TAG, "Marketplace proxy [socketfix-v14]: %s %s → %s (ok=%d, body=%zu bytes)",
             method_str, uri, target_path.c_str(), ok, resp_body.size());

    // Cache successful GET responses so repeated polls don't re-proxy.
    if (is_get && ok && !resp_body.empty()) {
        mpx_cache_put(target_path, resp_body);
    }

    if (!ok) {
        // Pass the gateway's own status through. Collapsing everything to 502
        // meant "you do not own this skill" (403) and "no such skill" (404)
        // both surfaced in the UI as "Failed to ...: 502", which reads as a
        // broken robot rather than as the answer it actually is. 502 is now
        // reserved for what it means: we could not reach the gateway.
        if (resp_body.empty() || upstream_status == 0) {
            ESP_LOGW(TAG, "Gateway unreachable for %s", target_path.c_str());
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"gateway unreachable\"}", -1);
        } else {
            ESP_LOGW(TAG, "Gateway %d for %s: %.*s", upstream_status,
                     target_path.c_str(),
                     (int)std::min(resp_body.size(), size_t(256)),
                     resp_body.c_str());
            char line[32];
            std::snprintf(line, sizeof(line), "%d Upstream", upstream_status);
            httpd_resp_set_status(req, line);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, resp_body.c_str(), resp_body.size());
        }
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_body.c_str(), resp_body.size());
    return ESP_OK;
}

/* ── Dead-connection reaper ──────────────────────────────────────
 * The socket census proved the pool was exhausted by DEAD browser
 * connections on port 80 (peer=0.0.0.0) that httpd never freed.  This task
 * periodically peeks every port-80 socket; if the peer has closed (recv
 * returns 0 / the socket is no longer connected), it asks httpd to close that
 * session.  Using httpd_sess_trigger_close (not a raw close) keeps it safe —
 * httpd owns the actual close and ignores fds that aren't its sessions.
 */
static void socket_reaper_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(4000));
        if (!s_server) continue;

        int reaped = 0, zombies = 0;
        for (int fd = LWIP_SOCKET_OFFSET;
             fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
            int type = 0; socklen_t tl = sizeof(type);
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0) continue;

            struct sockaddr_in local {}; socklen_t ll = sizeof(local);
            bool has_local = (getsockname(fd, (struct sockaddr *)&local, &ll) == 0);

            // ── Zombie: an open socket whose TCP PCB is gone (getsockname
            // fails → no address).  These are aborted/RST connections that
            // leaked.  A freshly created-but-unconnected socket still reports
            // 0.0.0.0:0 (success), so it is NOT mistaken for a zombie.
            if (!has_local) {
                close(fd);            // single close — reclaim the fd
                zombies++;
                continue;
            }

            if (ntohs(local.sin_port) != 80) continue;   // only the web server

            // Listener has no connected peer — never reap it.
            struct sockaddr_in peer {}; socklen_t pl = sizeof(peer);
            if (getpeername(fd, (struct sockaddr *)&peer, &pl) != 0) continue;

            // Peek one byte, non-blocking.  0 = peer closed (EOF); a hard
            // error other than "would block" also means the link is dead.
            // A live/idle WS client returns EWOULDBLOCK and is left alone.
            char probe;
            int r = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            bool dead = (r == 0) ||
                        (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN);
            if (dead) {
                close(fd);            // single close — peer already gone
                reaped++;
            }
        }
        if (reaped || zombies) {
            ESP_LOGW(TAG, "socket_reaper: reclaimed %d dead + %d zombie socket(s)",
                     reaped, zombies);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────── */

bool start_http_server()
{
    if (s_server) {
        ESP_LOGW(TAG, "HTTP server already running");
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 64;   // +11 Servo Studio routes, +2 studio page
    // LWIP has CONFIG_LWIP_MAX_SOCKETS (16) total.  HTTPD also reserves 3
    // internal sockets on top of max_open_sockets.  At 5, HTTPD uses at most
    // 5+3=8, GUARANTEEING ~8 sockets stay free for OUTBOUND use (upstream cloud
    // WebSocket, marketplace proxy, DNS) plus the PWA WS clients.  A browser
    // opens up to ~6 persistent connections per tab, so a lower cap here forces
    // it to reuse/cycle connections instead of pinning the whole pool.  With
    // lru_purge_enable the oldest idle connection is dropped when the cap is hit
    // rather than starving everything else.
    config.max_open_sockets = 5;
    config.stack_size = 8192;
    config.task_priority = 6;            // Priority 6 (per REQ-ROB-02)
    config.core_id = 0;                  // Pin to Core 0 (PRO_CPU)
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;  // Enable wildcard (*) URIs
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 60;       // Allow idle WS up to 60 s between frames
    config.send_wait_timeout = 30;       // Allow slow sends (e.g. large replies)
    config.close_fn = pwa_release_fd;    // Clean up PWA client slots on session close

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }

    // ── BUILD MARKER — confirms which firmware is actually running ──
    ESP_LOGW(TAG, "==== MPX BUILD socketfix-v14 : max_open_sockets=%d ====",
             config.max_open_sockets);

    // Start the dead-connection reaper so leaked port-80 sockets can't
    // accumulate and exhaust the pool.
    xTaskCreate(socket_reaper_task, "sock_reaper", 3072, nullptr, 3, nullptr);

    // ── Helper to register a static file handler ──
    auto register_static = [&](const char *uri) {
        httpd_uri_t h = {
            .uri       = uri,
            .method    = HTTP_GET,
            .handler   = static_handler,
            .user_ctx  = nullptr,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
        };
        httpd_register_uri_handler(s_server, &h);
    };

    // ── Register handlers for each known static file ──
    register_static("/");
    register_static("/index.html");
    register_static("/m.js");
    register_static("/index.css");
    register_static("/manifest.json");
    register_static("/sw.js");
    register_static("/favicon.ico");
    register_static("/studio");
    register_static("/studio.html");
#ifdef CONFIG_PWA_DESIGN_REDESIGN
    register_static("/md.svg");
    register_static("/eye-open.svg");
    register_static("/eye-close.svg");
#else
    register_static("/icon.svg");
#endif

    // ── Register WebSocket endpoint (telemetry) ──
    httpd_uri_t ws_uri = {
        .uri       = "/v1/telemetry/stream",
        .method    = HTTP_GET,
        .handler   = websocket_handler,
        .user_ctx  = nullptr,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    // ── MangDang Servo Studio ──
    register_api(s_server, "GET", "/v1/studio/mode",    HTTP_GET, studio_mode_handler);
    register_api(s_server, "GET", "/v1/studio/status",  HTTP_GET, studio_status_handler);
    register_api(s_server, "GET", "/v1/studio/scan",    HTTP_GET, studio_scan_handler);
    register_api(s_server, "GET", "/v1/studio/dump",    HTTP_GET, studio_dump_handler);
    register_api(s_server, "GET", "/v1/studio/set",     HTTP_GET, studio_set_handler);
    register_api(s_server, "GET", "/v1/studio/setall",  HTTP_GET, studio_setall_handler);
    register_api(s_server, "GET", "/v1/studio/live",    HTTP_GET, studio_live_handler);
    register_api(s_server, "GET", "/v1/studio/temps",   HTTP_GET, studio_temps_handler);
    register_api(s_server, "GET", "/v1/studio/save",    HTTP_GET, studio_save_handler);
    register_api(s_server, "GET", "/v1/studio/restore", HTTP_GET, studio_restore_handler);
    register_api(s_server, "GET", "/v1/studio/direct",  HTTP_GET, studio_direct_handler);

    // ── Register Chat WebSocket endpoint ──
    httpd_uri_t chat_ws_uri = {
        .uri       = "/v1/chat/ui",
        .method    = HTTP_GET,
        .handler   = chat_ws_handler,
        .user_ctx  = nullptr,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(s_server, &chat_ws_uri);

    // ── Register REST API endpoints ──
    register_api(s_server, "GET",  "/v1/skills/list",   HTTP_GET,  api_skills_list);
    register_api(s_server, "POST", "/v1/skills/run",    HTTP_POST, api_skills_run);
    register_api(s_server, "GET",  "/v1/skills/status", HTTP_GET,  api_skills_status);
    register_api(s_server, "GET",  "/v1/logs",          HTTP_GET,  api_logs);
    register_api(s_server, "GET",  "/v1/skills/installed", HTTP_GET,  api_skills_installed);
    register_api(s_server, "POST", "/v1/skills/uninstall",  HTTP_POST, api_skills_uninstall);
    register_api(s_server, "GET",  "/v1/fs/list",       HTTP_GET,  api_fs_list);
    register_api(s_server, "GET",  "/v1/fs/info",       HTTP_GET,  api_fs_info);
    register_api(s_server, "GET",  "/v1/fs/read",       HTTP_GET,  api_fs_read);
    register_api(s_server, "POST", "/v1/fs/delete",     HTTP_POST, api_fs_delete);
    register_api(s_server, "POST", "/v1/skills/upload", HTTP_POST, api_skills_upload);

    // ── Register robot control API endpoints ──
    register_api(s_server, "POST", "/v1/robot/gait",           HTTP_POST, api_robot_gait);
    register_api(s_server, "POST", "/v1/robot/joy",            HTTP_POST, api_robot_joy);
    register_api(s_server, "GET",  "/v1/robot/status",         HTTP_GET,  api_robot_status);
    register_api(s_server, "POST", "/v1/robot/config",         HTTP_POST, api_robot_config);
    register_api(s_server, "GET",  "/v1/robot/config",         HTTP_GET,  api_robot_get_config);
    register_api(s_server, "POST", "/v1/robot/calibrate",      HTTP_POST, api_robot_calibrate);
    register_api(s_server, "POST", "/v1/robot/calibrate/reset",HTTP_POST, api_robot_cal_reset);
    register_api(s_server, "POST", "/v1/robot/diagnostic/ping", HTTP_POST, api_robot_diag_ping);
    register_api(s_server, "POST", "/v1/robot/diagnostic/sweep",HTTP_POST, api_robot_diag_sweep);

    // ── Register WiFi config API endpoints ──
    register_api(s_server, "GET",  "/v1/wifi/status",          HTTP_GET,  api_wifi_status);
    register_api(s_server, "POST", "/v1/wifi/connect",         HTTP_POST, api_wifi_connect);
    register_api(s_server, "POST", "/v1/wifi/disconnect",      HTTP_POST, api_wifi_disconnect);
    register_api(s_server, "POST", "/v1/wifi/forget",          HTTP_POST, api_wifi_forget);
    register_api(s_server, "POST", "/v1/wifi/ap-config",       HTTP_POST, api_wifi_ap_config);

    // ── Register Lua async enqueue endpoint (non-blocking) ──
    register_api(s_server, "POST", "/v1/lua/enqueue",          HTTP_POST, api_lua_enqueue);

    // ── Register Lua API endpoints ──
    register_api(s_server, "POST", "/v1/lua/run",              HTTP_POST, api_lua_run);
    register_api(s_server, "GET",  "/v1/lua/list",             HTTP_GET,  api_lua_list);
    register_api(s_server, "POST", "/v1/lua/save",             HTTP_POST, api_lua_save);
    register_api(s_server, "GET",  "/v1/lua/read",             HTTP_GET,  api_lua_read);
    register_api(s_server, "POST", "/v1/lua/delete",           HTTP_POST, api_lua_delete);

    // ── Register Chat REST endpoint ──
    register_api(s_server, "POST", "/v1/chat/send",            HTTP_POST, chat_send_handler);

    // ── Register Marketplace Gateway endpoints ──
    register_api(s_server, "GET",  "/v1/gateway/config",             HTTP_GET,   api_gateway_config);

    // Wildcard: catch all /v1/marketplace/* paths
    {
        httpd_uri_t h = {
            .uri       = "/v1/marketplace/*",
            .method    = HTTP_GET,
            .handler   = api_marketplace_proxy,
            .user_ctx  = nullptr,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
        };
        httpd_register_uri_handler(s_server, &h);
        h.method = HTTP_POST;
        httpd_register_uri_handler(s_server, &h);
        h.method = HTTP_PATCH;
        httpd_register_uri_handler(s_server, &h);
        h.method = HTTP_DELETE;
        httpd_register_uri_handler(s_server, &h);
        ESP_LOGI(TAG, "  API: GET|POST|PATCH|DELETE /v1/marketplace/* (proxied to gateway)");
    }

    ESP_LOGI(TAG, "HTTP server running on port 80 (Core 0, Priority 6)");
    ESP_LOGI(TAG, "  → PWA:  http://%s/", AP_IP_ADDR);
    ESP_LOGI(TAG, "  → WS:   ws://%s/v1/telemetry/stream", AP_IP_ADDR);
    ESP_LOGI(TAG, "  → WS:   ws://%s/v1/chat/ui", AP_IP_ADDR);

    return true;
}

void stop_http_server()
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

}  // namespace network
