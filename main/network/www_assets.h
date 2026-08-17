#pragma once

#include <cstddef>
#include <cstdint>

namespace network {

/**
 * @brief Look up a PWA asset embedded in the firmware image.
 *
 * Assets are embedded pre-gzipped ONLY — there is no uncompressed copy on the
 * device, and nothing is copied to LittleFS. The returned pointer addresses
 * memory-mapped flash, so serving it costs no heap.
 *
 * The caller MUST send the bytes with `Content-Encoding: gzip`.
 *
 * @param name    Logical path relative to the www root, e.g. "/index.css"
 *                (no ".gz" suffix).
 * @param out_len Receives the compressed length in bytes (may be nullptr).
 * @return Pointer to read-only flash data, or nullptr if there is no asset
 *         with that name.
 */
const uint8_t *embedded_asset(const char *name, std::size_t *out_len);

/**
 * @brief Number of embedded PWA assets.
 */
int embedded_asset_count();

/**
 * @brief Total compressed size of all embedded PWA assets, in bytes.
 */
std::size_t embedded_asset_bytes();

}  // namespace network
