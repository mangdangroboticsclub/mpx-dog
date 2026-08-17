#include "network/www_assets.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "www_assets";

/* ── Embedded binary symbols (linked via EMBED_FILES in CMakeLists) ──
 *
 * Only the pre-gzipped variant of each asset is embedded. Shipping both copies
 * cost ~389 KB of the app partition and bought nothing: every HTTP client that
 * matters sends `Accept-Encoding: gzip`, and the server always responds with
 * `Content-Encoding: gzip`.
 *
 * Assets are also never copied to LittleFS. They live in the app image, which
 * is memory-mapped, so the HTTP server hands the pointer straight to lwip —
 * no filesystem, no heap, no per-boot flash rewrite.
 *
 * NOTE: symbol names come from the filename only (path prefix stripped, every
 * non-alphanumeric character replaced with '_'), so "index.css.gz" becomes
 * _binary_index_css_gz_start / _end.
 */

#define DECLARE_EMBED(name)                                                  \
    extern const uint8_t _binary_##name##_start[];                           \
    extern const uint8_t _binary_##name##_end[]

// ── Common assets (present in both designs) ──────────────────
DECLARE_EMBED(index_html_gz);
DECLARE_EMBED(index_css_gz);
DECLARE_EMBED(m_js_gz);
DECLARE_EMBED(manifest_json_gz);
DECLARE_EMBED(sw_js_gz);

// ── MangDang Servo Studio (standalone page, ../studio/studio.html) ──
DECLARE_EMBED(studio_html_gz);

// ── Design-specific assets ───────────────────────────────────
#ifdef CONFIG_PWA_DESIGN_REDESIGN
DECLARE_EMBED(md_svg_gz);
DECLARE_EMBED(eye_open_svg_gz);
DECLARE_EMBED(eye_close_svg_gz);
#else
DECLARE_EMBED(icon_svg_gz);
#endif

/* ── Asset table ────────────────────────────────────────────── */

namespace {

struct AssetFile {
    const char *name;              // logical path, e.g. "/index.css"
    const uint8_t *start;          // gzip stream in mapped flash
    const uint8_t *end;

    std::size_t size() const { return static_cast<std::size_t>(end - start); }
};

// Helper macro to avoid repetition in the table
#define ASSET_ENTRY(path, sym)  { path, _binary_##sym##_start, _binary_##sym##_end }

const AssetFile ASSETS[] = {
    // ── Common ───────────────────────────────────────────────
    ASSET_ENTRY("/index.html",    index_html_gz),
    ASSET_ENTRY("/index.css",     index_css_gz),
    ASSET_ENTRY("/m.js",          m_js_gz),
    ASSET_ENTRY("/manifest.json", manifest_json_gz),
    ASSET_ENTRY("/sw.js",         sw_js_gz),

    // ── Servo Studio ─────────────────────────────────────────
    // Deliberately NOT part of the Svelte bundle: this is the page you need
    // when the robot will not walk, so it must not depend on the app build.
    ASSET_ENTRY("/studio.html",   studio_html_gz),

    // ── Design-specific ──────────────────────────────────────
#ifdef CONFIG_PWA_DESIGN_REDESIGN
    ASSET_ENTRY("/md.svg",        md_svg_gz),
    ASSET_ENTRY("/eye-open.svg",  eye_open_svg_gz),
    ASSET_ENTRY("/eye-close.svg", eye_close_svg_gz),
#else
    ASSET_ENTRY("/icon.svg",      icon_svg_gz),
#endif
};

constexpr int NUM_ASSETS = sizeof(ASSETS) / sizeof(ASSETS[0]);

}  // namespace

/* ── Public API ─────────────────────────────────────────────── */

const uint8_t *network::embedded_asset(const char *name, std::size_t *out_len)
{
    if (!name) {
        return nullptr;
    }

    for (int i = 0; i < NUM_ASSETS; i++) {
        if (std::strcmp(ASSETS[i].name, name) != 0) {
            continue;
        }
        const std::size_t sz = ASSETS[i].size();
        if (sz == 0) {
            ESP_LOGW(TAG, "Embedded asset %s is empty", name);
            return nullptr;
        }
        if (out_len) {
            *out_len = sz;
        }
        return ASSETS[i].start;
    }
    return nullptr;
}

int network::embedded_asset_count()
{
    return NUM_ASSETS;
}

std::size_t network::embedded_asset_bytes()
{
    std::size_t sum = 0;
    for (int i = 0; i < NUM_ASSETS; i++) {
        sum += ASSETS[i].size();
    }
    return sum;
}
