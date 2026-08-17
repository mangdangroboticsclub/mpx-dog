#pragma once

#include <cstddef>
#include <string>

namespace network {

/**
 * @brief Make an HTTP request to the marketplace Gateway and return the
 *        response body as a string.
 *
 * Opens a TCP connection to the Gateway host (from Kconfig) on the STA
 * interface, sends an HTTP request with the given method, path, and
 * optional body, then reads and returns the full response body.
 *
 * @param method  HTTP method: "GET", "POST", "PATCH", "DELETE"
 * @param path    URL path on the Gateway, e.g. "/v1/skills" or
 *                "/v1/robots/MPX-DOG-01/skills"
 * @param body    Request body (empty for GET/DELETE)
 * @param ok      [out] set to true if the server returned 2xx
 * @param status_out [out, optional] the upstream HTTP status code, or 0 if the
 *                gateway could not be reached at all. Without this the proxy
 *                could only answer 502 for everything, so "you do not own this
 *                skill" (403) and "no such skill" (404) both reached the UI as
 *                "Failed to ...: 502" — the caller had no way to tell a real
 *                answer from a broken connection.
 * @return        Response body string, or empty on failure
 */
std::string gateway_request(const char *method,
                            const char *path,
                            const std::string &body,
                            bool &ok,
                            int *status_out = nullptr);

}  // namespace network
