/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_CONFIG_PROVIDER_H
#define AG_LOCAL_SERVER_CONFIG_PROVIDER_H

#include "types/local_config.h"
#include "types/local_server_result.h"

// Product-supplied config semantics for GET / PUT /api/v1/config.
//
// Ownership : product owns the implementation.
// Lifetime  : must outlive the LocalServer.
// Thread-safe: yes — called from the httpd task.
class ConfigProvider {
public:
  virtual ~ConfigProvider() = default;

  // Current settings mapped into the flat schema for GET /api/v1/config.
  // Unsupported fields are std::nullopt and omitted from the JSON.
  virtual LocalServerConfig get_config() = 0;

  // Validate and submit a partial config (only present fields set). Submission
  // MUST be non-blocking and all-or-nothing: validate every present field
  // first (range, enum, model support, source gate), then atomically admit the
  // complete update for later product-owned processing. Do not perform NVS,
  // sensor, display, cloud, or other potentially blocking work here.
  //
  // Accepted means the product assumed responsibility for processing; it does
  // not guarantee persistence or runtime application. Clients confirm eventual
  // state through GET. An empty object still evaluates provider policy and is
  // accepted as a no-op without consuming product queue capacity when writes
  // are enabled.
  virtual ConfigSubmitResult submit_config(const LocalServerConfig &partial) = 0;
};

#endif // AG_LOCAL_SERVER_CONFIG_PROVIDER_H
