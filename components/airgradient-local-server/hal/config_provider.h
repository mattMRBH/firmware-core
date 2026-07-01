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

  // Validate and apply a partial config (only present fields set). MUST be
  // all-or-nothing: validate every present field first (range, enum, model
  // support, configuration_control gate); if any field fails, persist and
  // apply NOTHING and return the failing field. Only after full validation
  // passes may it persist and apply. A rejected PUT therefore never leaves
  // some fields changed.
  //
  // Products MUST funnel local config writers (HTTP, BLE, UI) into one
  // internal apply path so the channels cannot drift.
  virtual ConfigApplyResult apply_config(const LocalServerConfig &partial) = 0;
};

#endif // AG_LOCAL_SERVER_CONFIG_PROVIDER_H
