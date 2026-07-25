/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/local_server.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "ag_log.h"
#include "internal/config_json.h"
#include "internal/measures_json.h"

namespace {

constexpr const char *TAG = "LocalServer";

// All route paths are static-lifetime literals; OwnedRoute borrows them.
constexpr const char *PATH_MEASURES = "/api/v1/measures";
constexpr const char *PATH_CONFIG = "/api/v1/config";
constexpr const char *PATH_ACTION_CALIBRATE_CO2 = "/api/v1/actions/calibrate-co2";
constexpr const char *PATH_ACTION_TEST_LEDS = "/api/v1/actions/test-leds";

// Includes headroom for a fully escaped MAX_UNKNOWN_KEY field.
constexpr size_t ERROR_BUF_SIZE = 512;
constexpr const char *FALLBACK_INVALID_BODY =
    R"({"error":{"code":"invalid_body","message":"invalid request body"}})";
constexpr const char *FALLBACK_UNKNOWN_FIELD =
    R"({"error":{"code":"unknown_field","message":"unknown field"}})";
constexpr const char *FALLBACK_INVALID_VALUE =
    R"({"error":{"code":"invalid_value","message":"invalid value"}})";
constexpr const char *FALLBACK_FORBIDDEN =
    R"({"error":{"code":"forbidden","message":"forbidden"}})";
constexpr const char *FALLBACK_NOT_FOUND =
    R"({"error":{"code":"not_found","message":"not found"}})";
constexpr const char *FALLBACK_BUSY = R"({"error":{"code":"busy","message":"busy"}})";
constexpr const char *FALLBACK_INTERNAL =
    R"({"error":{"code":"internal","message":"internal error"}})";

const char *fallback_error_body(ApiErrorCode code) {
  switch (code) {
  case ApiErrorCode::InvalidBody:
    return FALLBACK_INVALID_BODY;
  case ApiErrorCode::UnknownField:
    return FALLBACK_UNKNOWN_FIELD;
  case ApiErrorCode::InvalidValue:
    return FALLBACK_INVALID_VALUE;
  case ApiErrorCode::Forbidden:
    return FALLBACK_FORBIDDEN;
  case ApiErrorCode::NotFound:
    return FALLBACK_NOT_FOUND;
  case ApiErrorCode::Busy:
    return FALLBACK_BUSY;
  case ApiErrorCode::Internal:
    return FALLBACK_INTERNAL;
  }
  return FALLBACK_INTERNAL;
}

bool append_text(char *buf, size_t size, size_t &offset, const char *text) {
  const size_t length = std::strlen(text);
  if (length >= size - offset) {
    return false;
  }
  std::memcpy(buf + offset, text, length);
  offset += length;
  buf[offset] = '\0';
  return true;
}

bool append_escaped_json(char *buf, size_t size, size_t &offset, const char *text) {
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p != '\0'; ++p) {
    const char *escape = nullptr;
    switch (*p) {
    case '"':
      escape = "\\\"";
      break;
    case '\\':
      escape = "\\\\";
      break;
    case '\b':
      escape = "\\b";
      break;
    case '\f':
      escape = "\\f";
      break;
    case '\n':
      escape = "\\n";
      break;
    case '\r':
      escape = "\\r";
      break;
    case '\t':
      escape = "\\t";
      break;
    default:
      break;
    }

    if (escape != nullptr) {
      if (!append_text(buf, size, offset, escape)) {
        return false;
      }
    } else if (*p < 0x20) {
      char unicode_escape[7];
      std::snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *p);
      if (!append_text(buf, size, offset, unicode_escape)) {
        return false;
      }
    } else {
      char character[2] = {static_cast<char>(*p), '\0'};
      if (!append_text(buf, size, offset, character)) {
        return false;
      }
    }
  }
  return true;
}

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
  const char *fallback = fallback_error_body(code);

  char buf[ERROR_BUF_SIZE] = {};
  size_t offset = 0;
  bool ok = append_text(buf, sizeof(buf), offset, R"({"error":{"code":")") &&
            append_text(buf, sizeof(buf), offset, api_error_code_str(code)) &&
            append_text(buf, sizeof(buf), offset, "\"");
  if (ok && field != nullptr && field[0] != '\0') {
    ok = append_text(buf, sizeof(buf), offset, R"(,"field":")") &&
         append_escaped_json(buf, sizeof(buf), offset, field) &&
         append_text(buf, sizeof(buf), offset, "\"");
  }
  ok = ok && append_text(buf, sizeof(buf), offset, R"(,"message":")") &&
       append_text(buf, sizeof(buf), offset, api_error_message(code)) &&
       append_text(buf, sizeof(buf), offset, R"("}})");
  resp.json(status, ok ? buf : fallback);
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

  if (!req.body_complete()) {
    _send_error(resp, ApiErrorCode::InvalidBody, nullptr);
    return;
  }

  const char *body = req.body();
  const size_t body_length = req.body_length();
  AG_LOGI(TAG, "PUT config body: %.*s", static_cast<int>(body_length), body != nullptr ? body : "");

  LocalServerConfig partial;
  const config_json::ParseResult pr = config_json::parse(body, body_length, partial);
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

  const ConfigSubmitResult result = _config->submit_config(partial);
  switch (result.status) {
  case ConfigSubmitStatus::Accepted:
    resp.empty(HttpStatus::Accepted);
    return;
  case ConfigSubmitStatus::InvalidValue:
    _send_error(resp, ApiErrorCode::InvalidValue, config_json::config_field_wire_key(result.field));
    return;
  case ConfigSubmitStatus::Forbidden:
    _send_error(resp, ApiErrorCode::Forbidden, nullptr);
    return;
  case ConfigSubmitStatus::NotSupported:
    _send_error(resp, ApiErrorCode::NotFound, config_json::config_field_wire_key(result.field));
    return;
  case ConfigSubmitStatus::Busy:
    _send_error(resp, ApiErrorCode::Busy, nullptr);
    return;
  case ConfigSubmitStatus::Internal:
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
    resp.empty(HttpStatus::Ok);
    return;
  case ActionStatus::Rejected:
    _send_error(resp, ApiErrorCode::Forbidden, nullptr);
    return;
  case ActionStatus::NotSupported:
    _send_error(resp, ApiErrorCode::NotFound, nullptr);
    return;
  case ActionStatus::Busy:
    _send_error(resp, ApiErrorCode::Busy, nullptr);
    return;
  }
}
