#include "fs/littlefs_manager.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "littlefs";

static constexpr const char *PARTITION_LABEL = "storage";
static constexpr const char *MOUNT_POINT     = "/fs";

namespace fs {

bool init_littlefs()
{
	ESP_LOGI(TAG, "Mounting LittleFS on partition '%s' at '%s'", PARTITION_LABEL, MOUNT_POINT);

	esp_vfs_littlefs_conf_t conf = {};
	conf.base_path = MOUNT_POINT;
	conf.partition_label = PARTITION_LABEL;
	conf.format_if_mount_failed = true;
	conf.dont_mount = false;

	const esp_err_t ret = esp_vfs_littlefs_register(&conf);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to mount LittleFS (err=%s)", esp_err_to_name(ret));
		return false;
	}

	// Log partition info
	size_t total = 0, used = 0;
	if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
		ESP_LOGI(TAG, "LittleFS mounted: total=%zukB, used=%zukB",
				 total / 1024, used / 1024);
	}

	return true;
}

void deinit_littlefs()
{
	const esp_err_t ret = esp_vfs_littlefs_unregister(PARTITION_LABEL);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "Failed to unregister LittleFS (err=%s)", esp_err_to_name(ret));
	}
}

bool write_file(const char *path, const void *data, std::size_t len)
{
	// Build full path
	char full_path[320];
	std::snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

	FILE *f = fopen(full_path, "wb");
	if (!f) {
		ESP_LOGE(TAG, "Failed to open '%s' for writing", full_path);
		return false;
	}

	const size_t written = fwrite(data, 1, len, f);
	fclose(f);

	// Even a short write consumed blocks, so invalidate before the error path.
	invalidate_stats();

	if (written != len) {
		ESP_LOGE(TAG, "Short write to '%s': %zu/%zu bytes", full_path, written, len);
		return false;
	}

	ESP_LOGI(TAG, "Written %zu bytes to '%s'", len, full_path);
	return true;
}

std::vector<uint8_t> read_file(const char *path)
{
	char full_path[320];
	std::snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

	FILE *f = fopen(full_path, "rb");
	if (!f) {
		ESP_LOGW(TAG, "File '%s' not found", full_path);
		return {};
	}

	// Get file size
	fseek(f, 0, SEEK_END);
	const long file_len = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_len <= 0) {
		fclose(f);
		return {};
	}

	std::vector<uint8_t> buf(static_cast<std::size_t>(file_len));
	const size_t read = fread(buf.data(), 1, buf.size(), f);
	fclose(f);

	if (read != static_cast<size_t>(file_len)) {
		ESP_LOGE(TAG, "Short read from '%s': %zu/%ld bytes", full_path, read, file_len);
		return {};
	}

	return buf;
}

bool delete_file(const char *path)
{
	char full_path[320];
	std::snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

	if (unlink(full_path) != 0) {
		if (errno == ENOENT) {
			return true;  // Already gone — nothing freed, cache still good
		}
		ESP_LOGE(TAG, "Failed to delete '%s': errno=%d", full_path, errno);
		return false;
	}
	invalidate_stats();

	ESP_LOGI(TAG, "Deleted '%s'", full_path);
	return true;
}

bool file_exists(const char *path)
{
	char full_path[320];
	std::snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

	FILE *f = fopen(full_path, "rb");
	if (f) {
		fclose(f);
		return true;
	}
	return false;
}

std::size_t file_size(const char *path)
{
	char full_path[320];
	std::snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

	FILE *f = fopen(full_path, "rb");
	if (!f) {
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long len = ftell(f);
	fclose(f);
	return (len > 0) ? static_cast<std::size_t>(len) : 0;
}

std::vector<std::string> list_files(const char *dir_path)
{
	std::vector<std::string> result;
	char full_path[320];
	std::snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, dir_path);

	DIR *dir = opendir(full_path);
	if (!dir) {
		ESP_LOGW(TAG, "Failed to open directory '%s'", full_path);
		return result;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) {
		if (entry->d_type == DT_REG) {
			char buf[320];
			std::snprintf(buf, sizeof(buf), "/%s", entry->d_name);
			result.emplace_back(buf);
		}
	}
	closedir(dir);

	return result;
}

/* ── Cached partition stats ──────────────────────────────────────
 * See the header for why this is cached rather than measured per call.
 * No lock: the two values are written together and only ever read as a pair
 * by tasks that tolerate a stale reading by design; the worst outcome of a
 * race is one request seeing the previous figures.
 */
static std::size_t s_stat_total = 0;
static std::size_t s_stat_used  = 0;
static bool        s_stat_valid = false;
static TickType_t  s_stat_at    = 0;

/* Long enough that a burst of requests measures once, short enough that a
 * write from outside this module is not remembered wrongly for long. */
static constexpr TickType_t STAT_TTL = pdMS_TO_TICKS(15000);

void invalidate_stats()
{
	s_stat_valid = false;
}

bool stats_fresh(std::size_t &total_bytes, std::size_t &used_bytes)
{
	std::size_t t = 0, u = 0;
	if (esp_littlefs_info(PARTITION_LABEL, &t, &u) != ESP_OK) return false;

	s_stat_total = t;
	s_stat_used  = u;
	s_stat_at    = xTaskGetTickCount();
	s_stat_valid = true;

	total_bytes = t;
	used_bytes  = u;
	return true;
}

bool stats(std::size_t &total_bytes, std::size_t &used_bytes)
{
	if (s_stat_valid && (xTaskGetTickCount() - s_stat_at) < STAT_TTL) {
		total_bytes = s_stat_total;
		used_bytes  = s_stat_used;
		return true;
	}
	return stats_fresh(total_bytes, used_bytes);
}

}  // namespace fs
