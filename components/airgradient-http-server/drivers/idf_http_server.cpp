/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "idf_http_server.h"

#include <cstddef>
#include <cstring>
#include <string>

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include "hal/http_request.h"
#include "hal/http_response.h"
#include "types/http_types.h"

namespace {

static constexpr const char *TAG = "IdfHttpServer";

// Concrete HttpRequest wrapping httpd_req_t*. Constructed on the stack
// inside the trampoline. Body is buffered lazily on first access.
class IdfHttpRequest : public HttpRequest {
public:
  IdfHttpRequest(httpd_req_t *req, HttpMethod method) : _req(req), _method(method) {}

  HttpMethod method() const override { return _method; }
  const char *uri() const override { return _req->uri; }

  const char *body() const override {
    _ensure_body();
    return _body.empty() ? nullptr : _body.c_str();
  }

  size_t body_length() const override {
    _ensure_body();
    return _body.size();
  }

  bool get_header(const char *name, char *buf, size_t buf_len) const override {
    if (name == nullptr || buf == nullptr || buf_len == 0) {
      return false;
    }
    const size_t value_len = httpd_req_get_hdr_value_len(_req, name);
    if (value_len == 0) {
      return false;
    }
    // httpd_req_get_hdr_value_str expects buf_len that includes the
    // NUL terminator and returns ESP_OK when the buffer is large enough.
    if (httpd_req_get_hdr_value_str(_req, name, buf, buf_len) != ESP_OK) {
      // Buffer too small — truncate gracefully.
      buf[buf_len - 1] = '\0';
      return false;
    }
    return true;
  }

  bool get_query_param(const char *key, char *buf, size_t buf_len) const override {
    if (key == nullptr || buf == nullptr || buf_len == 0) {
      return false;
    }
    const size_t qs_len = httpd_req_get_url_query_len(_req);
    if (qs_len == 0) {
      return false;
    }
    std::string qs;
    qs.resize(qs_len + 1);
    if (httpd_req_get_url_query_str(_req, qs.data(), qs.size()) != ESP_OK) {
      return false;
    }
    if (httpd_query_key_value(qs.c_str(), key, buf, buf_len) != ESP_OK) {
      return false;
    }
    return true;
  }

private:
  void _ensure_body() const {
    if (_body_loaded) {
      return;
    }
    _body_loaded = true;
    const size_t total = _req->content_len;
    if (total == 0) {
      return;
    }
    const size_t cap = static_cast<size_t>(CONFIG_AG_HTTP_MAX_BODY_SIZE);
    if (total > cap) {
      ESP_LOGW(TAG, "request body %u bytes exceeds cap %u, truncating",
               static_cast<unsigned>(total), static_cast<unsigned>(cap));
    }
    const size_t to_read = total > cap ? cap : total;
    _body.resize(to_read);
    size_t off = 0;
    while (off < to_read) {
      const int r = httpd_req_recv(_req, _body.data() + off, to_read - off);
      if (r <= 0) {
        // Read failure or socket closed; keep whatever was buffered.
        _body.resize(off);
        return;
      }
      off += static_cast<size_t>(r);
    }
  }

  httpd_req_t *_req;
  HttpMethod _method;
  mutable std::string _body;
  mutable bool _body_loaded = false;
};

httpd_method_t to_httpd_method(HttpMethod m) {
  switch (m) {
  case HttpMethod::Get:
    return HTTP_GET;
  case HttpMethod::Post:
    return HTTP_POST;
  case HttpMethod::Put:
    return HTTP_PUT;
  case HttpMethod::Delete:
    return HTTP_DELETE;
  }
  return HTTP_GET;
}

HttpMethod from_httpd_method(int m) {
  switch (m) {
  case HTTP_GET:
    return HttpMethod::Get;
  case HTTP_POST:
    return HttpMethod::Post;
  case HTTP_PUT:
    return HttpMethod::Put;
  case HTTP_DELETE:
    return HttpMethod::Delete;
  default:
    return HttpMethod::Get;
  }
}

const char *status_phrase(HttpStatus s) {
  switch (s) {
  case HttpStatus::Ok:
    return "200 OK";
  case HttpStatus::Created:
    return "201 Created";
  case HttpStatus::NoContent:
    return "204 No Content";
  case HttpStatus::BadRequest:
    return "400 Bad Request";
  case HttpStatus::NotFound:
    return "404 Not Found";
  case HttpStatus::MethodNotAllowed:
    return "405 Method Not Allowed";
  case HttpStatus::InternalServerError:
    return "500 Internal Server Error";
  }
  return "500 Internal Server Error";
}

} // namespace

IdfHttpServer::IdfHttpServer() = default;

IdfHttpServer::~IdfHttpServer() { stop(); }

bool IdfHttpServer::register_route(HttpMethod method, const char *path, HttpHandler handler) {
  if (_started) {
    ESP_LOGE(TAG, "register_route() called after start()");
    return false;
  }
  if (path == nullptr || handler == nullptr) {
    return false;
  }
  _routes.emplace_back(
      std::make_unique<Route>(Route{method, std::string(path), std::move(handler)}));
  return true;
}

bool IdfHttpServer::start(uint16_t port) {
  if (_started) {
    return true;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  cfg.max_open_sockets = CONFIG_AG_HTTP_MAX_CONNECTIONS;
  // Default uri matcher is exact match — keep that behaviour.

  if (httpd_start(&_handle, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed on port %u", static_cast<unsigned>(port));
    _handle = nullptr;
    return false;
  }

  for (const auto &route : _routes) {
    httpd_uri_t uri = {};
    uri.uri = route->path.c_str();
    uri.method = to_httpd_method(route->method);
    uri.handler = &IdfHttpServer::_trampoline;
    uri.user_ctx = route.get();
    if (httpd_register_uri_handler(_handle, &uri) != ESP_OK) {
      ESP_LOGE(TAG, "failed to register route %s", route->path.c_str());
      httpd_stop(_handle);
      _handle = nullptr;
      return false;
    }
  }

  _started = true;
  return true;
}

void IdfHttpServer::stop() {
  if (!_started) {
    return;
  }
  if (_handle != nullptr) {
    httpd_stop(_handle);
    _handle = nullptr;
  }
  _started = false;
}

esp_err_t IdfHttpServer::_trampoline(httpd_req_t *req) {
  Route *route = static_cast<Route *>(req->user_ctx);
  if (route == nullptr || route->handler == nullptr) {
    httpd_resp_set_status(req, status_phrase(HttpStatus::InternalServerError));
    httpd_resp_send(req, nullptr, 0);
    return ESP_FAIL;
  }

  IdfHttpRequest request(req, from_httpd_method(req->method));
  HttpResponse response;

  route->handler(request, response);

  httpd_resp_set_status(req, status_phrase(response.status));
  if (response.content_type != nullptr) {
    httpd_resp_set_type(req, response.content_type);
  }
  for (size_t i = 0; i < response.header_count; ++i) {
    const HttpHeader &h = response.headers[i];
    if (h.name != nullptr && h.value != nullptr) {
      httpd_resp_set_hdr(req, h.name, h.value);
    }
  }

  const char *body = static_cast<const char *>(response.body_data());
  const size_t len = response.body_size();
  // httpd_resp_send treats HTTPD_RESP_USE_STRLEN (-1) as "use strlen"; we
  // always pass an explicit length to support binary payloads.
  return httpd_resp_send(req, body == nullptr ? "" : body, static_cast<ssize_t>(len));
}
