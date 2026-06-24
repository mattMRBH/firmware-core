/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/local_server.h"

#include <utility>

#include <cJSON.h>

#include "ag_log.h"
#include "internal/config_json.h"
#include "internal/measures_json.h"

namespace {

constexpr const char *TAG = "LocalServer";

// All route paths are static-lifetime literals; OwnedRoute borrows them.
constexpr const char *PATH_MEASURES = "/api/v1/measures";
constexpr const char *PATH_CONFIG = "/api/v1/config";
constexpr const char *PATH_ACTION_CALIBRATE_CO2 = "/api/v1/actions/calibrate_co2";
constexpr const char *PATH_ACTION_TEST_LEDS = "/api/v1/actions/test_leds";

// Worst-case structured error body is tiny (code + field + message).
constexpr size_t ERROR_BUF_SIZE = 192;
constexpr const char *FALLBACK_ERROR_BODY =
    R"({"error":{"code":"internal","message":"internal error"}})";

} // namespace

LocalServer::LocalServer(HttpServer &server, const Providers &providers)
    : _server(server), _measures(providers.measures), _config(providers.config),
      _config_access(providers.config_access), _actions(providers.actions) {}

LocalServer::~LocalServer() { end(); }

bool LocalServer::_register(HttpMethod method, const char *path, HttpHandler handler) {
  if (!_server.register_route(method, path, std::move(handler))) {
    return false;
  }
  _routes[_route_count] = {method, path};
  ++_route_count;
  return true;
}

bool LocalServer::begin() {
  if (_begun) {
    return true;
  }

  bool ok =
      _register(HttpMethod::Get, PATH_MEASURES,
                [this](const HttpRequest &q, HttpResponse &r) { _handle_get_measures(q, r); });

  if (ok && _config != nullptr && _config_access != ConfigAccess::Disabled) {
    ok = _register(HttpMethod::Get, PATH_CONFIG,
                   [this](const HttpRequest &q, HttpResponse &r) { _handle_get_config(q, r); });
    if (ok && _config_access == ConfigAccess::ReadWrite) {
      ok = _register(HttpMethod::Put, PATH_CONFIG,
                     [this](const HttpRequest &q, HttpResponse &r) { _handle_put_config(q, r); });
    }
  }

  if (ok && _actions != nullptr) {
    ok = _register(HttpMethod::Post, PATH_ACTION_CALIBRATE_CO2,
                   [this](const HttpRequest &q, HttpResponse &r) {
                     _handle_action(ActionId::CalibrateCo2, q, r);
                   });
    if (ok) {
      ok = _register(HttpMethod::Post, PATH_ACTION_TEST_LEDS,
                     [this](const HttpRequest &q, HttpResponse &r) {
                       _handle_action(ActionId::TestLeds, q, r);
                     });
    }
  }

  if (!ok) {
    AG_LOGE(TAG, "route registration failed; rolling back");
    end(); // unregister the routes registered so far in this call
    return false;
  }

  _begun = true;
  AG_LOGI(TAG, "registered %u local-server routes", static_cast<unsigned>(_route_count));
  return true;
}

void LocalServer::end() {
  for (size_t i = 0; i < _route_count; ++i) {
    _server.unregister_route(_routes[i].method, _routes[i].path);
  }
  _route_count = 0;
  _begun = false;
}

void LocalServer::_send_error(HttpResponse &resp, ApiErrorCode code, const char *field) {
  const HttpStatus status = api_error_status(code);

  cJSON *root = cJSON_CreateObject();
  cJSON *err = cJSON_CreateObject();
  if (root == nullptr || err == nullptr) {
    if (root != nullptr) {
      cJSON_Delete(root);
    }
    if (err != nullptr) {
      cJSON_Delete(err);
    }
    resp.json(status, FALLBACK_ERROR_BODY);
    return;
  }

  cJSON_AddStringToObject(err, "code", api_error_code_str(code));
  if (field != nullptr && field[0] != '\0') {
    cJSON_AddStringToObject(err, "field", field);
  }
  cJSON_AddStringToObject(err, "message", api_error_message(code));
  cJSON_AddItemToObject(root, "error", err);

  char buf[ERROR_BUF_SIZE];
  const bool ok = cJSON_PrintPreallocated(root, buf, sizeof(buf), /*format=*/0);
  cJSON_Delete(root);
  resp.json(status, ok ? buf : FALLBACK_ERROR_BODY);
}

void LocalServer::_handle_get_measures(const HttpRequest &, HttpResponse &resp) {
  const Measures m = _measures.get_measures();
  const SystemInfo info = _measures.get_system_info();
  const size_t len = measures_json::serialize(m, info, _json_buf, sizeof(_json_buf));
  if (len == 0) {
    AG_LOGE(TAG, "measures serialization failed");
    _send_error(resp, ApiErrorCode::Internal, nullptr);
    return;
  }
  // Borrowed (zero-copy): safe because httpd serializes requests and sends
  // the body synchronously before the buffer can be reused.
  resp.body_static(HttpStatus::Ok, _json_buf, len, "application/json");
}

void LocalServer::_handle_get_config(const HttpRequest &, HttpResponse &resp) {
  if (_config == nullptr) {
    _send_error(resp, ApiErrorCode::Internal, nullptr);
    return;
  }
  const LocalServerConfig cfg = _config->get_config();
  const size_t len = config_json::serialize(cfg, _json_buf, sizeof(_json_buf));
  if (len == 0) {
    AG_LOGE(TAG, "config serialization failed");
    _send_error(resp, ApiErrorCode::Internal, nullptr);
    return;
  }
  resp.body_static(HttpStatus::Ok, _json_buf, len, "application/json");
}

void LocalServer::_handle_put_config(const HttpRequest &req, HttpResponse &resp) {
  if (_config == nullptr) {
    _send_error(resp, ApiErrorCode::Internal, nullptr);
    return;
  }

  LocalServerConfig partial;
  const config_json::ParseResult pr = config_json::parse(req.body(), req.body_length(), partial);
  switch (pr.status) {
  case config_json::ParseStatus::InvalidBody:
    _send_error(resp, ApiErrorCode::InvalidBody, nullptr);
    return;
  case config_json::ParseStatus::UnknownField:
    _send_error(resp, ApiErrorCode::UnknownField, pr.unknown_key);
    return;
  case config_json::ParseStatus::InvalidValue:
    _send_error(resp, ApiErrorCode::InvalidValue, config_json::config_field_wire_key(pr.field));
    return;
  case config_json::ParseStatus::Ok:
    break;
  }

  const ConfigApplyResult result = _config->apply_config(partial);
  switch (result.status) {
  case ConfigApplyStatus::Ok:
    resp.no_content();
    return;
  case ConfigApplyStatus::InvalidValue:
    _send_error(resp, ApiErrorCode::InvalidValue, config_json::config_field_wire_key(result.field));
    return;
  case ConfigApplyStatus::Forbidden:
    _send_error(resp, ApiErrorCode::Forbidden, nullptr);
    return;
  case ConfigApplyStatus::NotSupported:
    _send_error(resp, ApiErrorCode::NotFound, config_json::config_field_wire_key(result.field));
    return;
  case ConfigApplyStatus::Internal:
    _send_error(resp, ApiErrorCode::Internal, nullptr);
    return;
  }
}

void LocalServer::_handle_action(ActionId action, const HttpRequest &, HttpResponse &resp) {
  if (_actions == nullptr) {
    _send_error(resp, ApiErrorCode::Internal, nullptr);
    return;
  }

  const ActionResult result = _actions->trigger(action);
  switch (result.status) {
  case ActionStatus::Dispatched:
    // Fire-and-forget: empty success body.
    resp.body(HttpStatus::Ok, "", 0, "application/json");
    return;
  case ActionStatus::Rejected:
    _send_error(resp, ApiErrorCode::Forbidden, nullptr);
    return;
  case ActionStatus::NotSupported:
    _send_error(resp, ApiErrorCode::NotFound, nullptr);
    return;
  }
}
