#include "network/www_assets.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fs/littlefs_manager.h"

static const char *TAG = "www_assets";

/* ── LittleFS mount point (must match fs/littlefs_manager.cc) ── */
static constexpr const char *MOUNT_POINT = "/fs";

/* ── Embedded binary symbols (linked via EMBED_FILES in CMakeLists) ── */

// Each www file is linked as _binary_<filename>_start / _end
// NOTE: symbol names are based on the filename only (path prefix stripped).

#define DECLARE_EMBED(name)                                                  \
    extern const uint8_t _binary_##name##_start[];                           \
    extern const uint8_t _binary_##name##_end[]

DECLARE_EMBED(icon_svg);
DECLARE_EMBED(icon_svg_gz);
DECLARE_EMBED(index_html);
DECLARE_EMBED(index_html_gz);
DECLARE_EMBED(index_css);
DECLARE_EMBED(index_css_gz);
DECLARE_EMBED(m_js);
DECLARE_EMBED(m_js_gz);
DECLARE_EMBED(manifest_json);
DECLARE_EMBED(manifest_json_gz);
DECLARE_EMBED(sw_js);
DECLARE_EMBED(sw_js_gz);

/* ── File descriptor table ──────────────────────────────────── */

struct AssetFile {
    const char *name;              // path within /fs/www/
    const uint8_t *start;
    const uint8_t *end;

    std::size_t size() const { return static_cast<std::size_t>(end - start); }
};

static const AssetFile ASSETS[] = {
    { "/icon.svg",        _binary_icon_svg_start,      _binary_icon_svg_end      },
    { "/icon.svg.gz",     _binary_icon_svg_gz_start,   _binary_icon_svg_gz_end   },
    { "/index.html",      _binary_index_html_start,    _binary_index_html_end    },
    { "/index.html.gz",   _binary_index_html_gz_start, _binary_index_html_gz_end },
    { "/index.css",       _binary_index_css_start,     _binary_index_css_end     },
    { "/index.css.gz",    _binary_index_css_gz_start,  _binary_index_css_gz_end  },
    { "/m.js",            _binary_m_js_start,           _binary_m_js_end          },
    { "/m.js.gz",         _binary_m_js_gz_start,       _binary_m_js_gz_end       },
    { "/manifest.json",   _binary_manifest_json_start, _binary_manifest_json_end  },
    { "/manifest.json.gz",_binary_manifest_json_gz_start,_binary_manifest_json_gz_end},
    { "/sw.js",           _binary_sw_js_start,         _binary_sw_js_end          },
    { "/sw.js.gz",        _binary_sw_js_gz_start,      _binary_sw_js_gz_end       },
};

static constexpr int NUM_ASSETS = sizeof(ASSETS) / sizeof(ASSETS[0]);

/* ── Public API ─────────────────────────────────────────────── */

bool network::deploy_www_assets()
{
    // Re-deploy assets every boot to pick up firmware-embedded updates.
    // The time cost (~50ms for 50KB) is negligible.

    // Create the /www/ subdirectory inside LittleFS
    {
        char dir_path[64];
        std::snprintf(dir_path, sizeof(dir_path), "%s/www", MOUNT_POINT);
        mkdir(dir_path, 0755);
        ESP_LOGI(TAG, "Ensured www directory exists: %s", dir_path);
    }

    size_t total_bytes = 0;
    int deployed = 0;

    for (int i = 0; i < NUM_ASSETS; i++) {
        const auto &a = ASSETS[i];
        const size_t sz = a.size();

        if (sz == 0) {
            ESP_LOGW(TAG, "Skipping empty asset: %s", a.name);
            continue;
        }

        // Build the full LittleFS path
        char littlefs_path[64];
        std::snprintf(littlefs_path, sizeof(littlefs_path), "/www%s", a.name);

        if (fs::write_file(littlefs_path, a.start, sz)) {
            total_bytes += sz;
            deployed++;
            ESP_LOGD(TAG, "  Deployed %s (%zu bytes)", a.name, sz);
        } else {
            ESP_LOGE(TAG, "  Failed to deploy %s", a.name);
            return false;
        }
    }

    ESP_LOGI(TAG, "Deployed %d PWA assets (%zu bytes) to LittleFS",
             deployed, total_bytes);

    return true;
}
