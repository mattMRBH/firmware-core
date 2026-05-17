/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_HTTP_CLIENT_H
#define AG_HTTP_CLIENT_H

#include <cstddef>
#include <cstdint>

// HttpClient is an internal interface consumed by AgClient.  Implementations
// wrap a concrete HTTP transport (e.g. esp_http_client).  All buffers are
// caller-owned; implementations must not retain pointers after the call
// returns.
//
// Execution context: task context only
// ISR-safe: no
// Thread-safe: no (single AgClient owner)
// Blocking: yes
class HttpClient {
public:
  virtual ~HttpClient() = default;

  // GET request.
  //
  // Returns true if the HTTP exchange completed (any status code).  Returns
  // false on transport failure (connection refused, DNS, TLS, timeout).
  //
  // Buffer contract (when return is true):
  //   - response fits           : *truncated=false, *bytes_written = body len
  //                               (excluding NUL); response_body is NUL-
  //                               terminated.
  //   - response exceeds buffer : *truncated=true, writes what fits, NUL-
  //                               terminates, *bytes_written = bytes written
  //                               (excluding NUL).
  //
  // Invalid arguments (body_size == 0 or response_body == nullptr) return
  // false with *bytes_written = 0.
  virtual bool get(const char *url, const char *cert_pem, int &status_code, char *response_body,
                   size_t body_size, size_t *bytes_written, bool *truncated) = 0;

  // POST request.  Returns true if the HTTP exchange completed, false on
  // transport failure.
  virtual bool post(const char *url, const char *cert_pem, const char *content_type,
                    const uint8_t *body, size_t body_len, int &status_code) = 0;
};

#endif // AG_HTTP_CLIENT_H
