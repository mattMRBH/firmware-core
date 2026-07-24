/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef DRIVERS_IDF_HTTP_REQUEST_H
#define DRIVERS_IDF_HTTP_REQUEST_H

#include <cstddef>
#include <string>

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include "http_body_reader.h"
#include "hal/http_request.h"
#include "types/http_types.h"

// Adapter that wraps an esp_http_server httpd_req_t* into the abstract
// HttpRequest interface consumed by HTTP handlers.
//
// Lifetime: constructed on the stack inside the driver's trampoline for
// the duration of a single request; never owned or named by user code.
// This header lives under drivers/ on purpose — consumers should depend on
// hal/http_request.h, not on this adapter.
//
// Body buffering is lazy and one-shot: the body is read on first access to
// body(), body_length(), or body_complete() and cached. Oversized or incomplete
// bodies are rejected without exposing a partial prefix.
//
// Header and query-parameter lookups are case-sensitive because the
// underlying esp_http_server accessors are case-sensitive.
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

  bool body_complete() const override {
    _ensure_body();
    return _body_status == http_internal::BodyReadStatus::Complete;
  }

  bool has_incomplete_body() const {
    return _body_loaded && _body_status != http_internal::BodyReadStatus::Complete;
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
  static constexpr const char *_TAG = "IdfHttpRequest";

  void _ensure_body() const {
    if (_body_loaded) {
      return;
    }
    _body_loaded = true;
    const size_t total = _req->content_len;
    const size_t cap = static_cast<size_t>(CONFIG_AG_HTTP_MAX_BODY_SIZE);
    if (total > cap) {
      ESP_LOGW(_TAG, "request body %u bytes exceeds cap %u, rejecting",
               static_cast<unsigned>(total), static_cast<unsigned>(cap));
    }
    _body_status = http_internal::read_body(_body, total, cap, [this](char *data, size_t length) {
      return httpd_req_recv(_req, data, length);
    });
    if (_body_status != http_internal::BodyReadStatus::Complete && total <= cap) {
      ESP_LOGW(_TAG, "request body ended before %u declared bytes", static_cast<unsigned>(total));
    }
  }

  httpd_req_t *_req;
  HttpMethod _method;
  mutable std::string _body;
  mutable bool _body_loaded = false;
  mutable http_internal::BodyReadStatus _body_status = http_internal::BodyReadStatus::ShortRead;
};

#endif // DRIVERS_IDF_HTTP_REQUEST_H
