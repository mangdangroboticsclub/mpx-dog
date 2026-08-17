#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fs {

/**
 * @brief Initialize and mount the LittleFS partition.
 *
 * If the partition contains valid LittleFS data it is mounted directly;
 * otherwise it is formatted first then mounted.
 *
 * @return true on success, false on failure.
 */
bool init_littlefs();

/**
 * @brief Unmount and release LittleFS resources.
 */
void deinit_littlefs();

/**
 * @brief Write data to a file, creating/overwriting it.
 *
 * @param path  Absolute path within the LittleFS mount (e.g. "/skill.wasm").
 * @param data  Pointer to the data buffer.
 * @param len   Number of bytes to write.
 * @return true on success.
 */
bool write_file(const char *path, const void *data, std::size_t len);

/**
 * @brief Read an entire file into memory.
 *
 * @param path  Absolute path within the LittleFS mount.
 * @return Vector of bytes, empty on failure.
 */
std::vector<uint8_t> read_file(const char *path);

/**
 * @brief Delete a file.
 *
 * @param path  Absolute path within the LittleFS mount.
 * @return true on success or if the file did not exist.
 */
bool delete_file(const char *path);

/**
 * @brief Check whether a file exists.
 */
bool file_exists(const char *path);

/**
 * @brief Return the size of a file in bytes, or 0 if it doesn't exist.
 */
std::size_t file_size(const char *path);

/**
 * @brief List all files under a given prefix path.
 *
 * Returns a vector of full paths (e.g. "/skill.wasm").
 */
std::vector<std::string> list_files(const char *dir_path);

/**
 * @brief Return total and used bytes on the LittleFS partition.
 *
 * Cached. The underlying `esp_littlefs_info()` calls `lfs_fs_size()`, which
 * walks every allocated block on a 13.4 MB partition reading flash as it goes
 * — hundreds of milliseconds to seconds, and it was being called on the HTTP
 * request path. esp_http_server serves requests from a single task, so that
 * traversal did not just delay the storage bar: it delayed whatever request
 * was queued behind it, which is why opening the Skills screen sometimes hung
 * for seconds.
 *
 * The value only changes when a file changes, and every write in this firmware
 * goes through write_file()/delete_file(), so both invalidate the cache. The
 * TTL is a backstop for anything that edits the partition behind our back.
 *
 * @return true if a value is available (cached or freshly measured).
 */
bool stats(std::size_t &total_bytes, std::size_t &used_bytes);

/**
 * @brief Measure the partition now, ignoring and refreshing the cache.
 *
 * Prefer stats(). Use this only where the exact current figure matters more
 * than the latency — it is the slow path by design.
 */
bool stats_fresh(std::size_t &total_bytes, std::size_t &used_bytes);

/**
 * @brief Mark the cached stats stale. Called by every write path here.
 */
void invalidate_stats();

}  // namespace fs
