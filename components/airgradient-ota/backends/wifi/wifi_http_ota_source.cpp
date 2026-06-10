/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "backends/wifi/wifi_http_ota_source.h"

#include "ag_log.h"
#include "services/ota_url.h"

#ifndef TEST_HOST
#include "esp_http_client.h"
#endif

#ifndef CONFIG_AG_OTA_HTTP_TIMEOUT_MS
#define CONFIG_AG_OTA_HTTP_TIMEOUT_MS 15000
#endif

namespace {
constexpr const char *TAG = "WifiHttpOtaSource";

constexpr int HTTP_OK = 200;
constexpr int HTTP_NOT_MODIFIED = 304;
constexpr int HTTP_BAD_REQUEST = 400;
constexpr int HTTP_NOT_FOUND = 404;
} // namespace

WifiHttpOtaSource::WifiHttpOtaSource(const OtaRequest &request) {
  if (ota_url::build(request, _url, sizeof(_url))) {
    _init_status = OtaStatus::Ok;
  } else {
    _url[0] = '\0';
    _init_status = OtaStatus::InvalidArgument;
    AG_LOGE(TAG, "failed to build firmware URL");
  }
}

WifiHttpOtaSource::~WifiHttpOtaSource() { close(); }

OtaStatus WifiHttpOtaSource::open(size_t *out_total_size) {
  if (out_total_size == nullptr) {
    return OtaStatus::InvalidArgument;
  }
  *out_total_size = 0;

  if (_init_status != OtaStatus::Ok) {
    return _init_status;
  }

#ifndef TEST_HOST
  esp_http_client_config_t config = {};
  config.url = _url;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = CONFIG_AG_OTA_HTTP_TIMEOUT_MS;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    AG_LOGE(TAG, "esp_http_client_init failed");
    return OtaStatus::TransportError;
  }
  _client = client;

  const esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    AG_LOGE(TAG, "esp_http_client_open failed: %s", esp_err_to_name(err));
    close();
    return OtaStatus::TransportError;
  }

  // Read the response headers so the status code and content length resolve.
  if (esp_http_client_fetch_headers(client) < 0) {
    AG_LOGE(TAG, "esp_http_client_fetch_headers failed");
    close();
    return OtaStatus::TransportError;
  }

  const int status_code = esp_http_client_get_status_code(client);
  AG_LOGI(TAG, "server returned status %d", status_code);

  switch (status_code) {
  case HTTP_OK: {
    const int content_length = esp_http_client_get_content_length(client);
    if (content_length > 0) {
      *out_total_size = static_cast<size_t>(content_length);
      return OtaStatus::Ok;
    }
    if (content_length == 0) {
      AG_LOGW(TAG, "server returned 200 with empty body");
      close();
      return OtaStatus::ServerError;
    }
    // content_length < 0: unknown / chunked. Size is known only at EOF.
    *out_total_size = 0;
    return OtaStatus::Ok;
  }
  case HTTP_NOT_MODIFIED:
    AG_LOGI(TAG, "firmware already up to date");
    close();
    return OtaStatus::UpToDate;
  case HTTP_BAD_REQUEST:
  case HTTP_NOT_FOUND:
    AG_LOGW(TAG, "server declined to serve an image");
    close();
    return OtaStatus::Declined;
  default:
    AG_LOGW(TAG, "unexpected status %d", status_code);
    close();
    return OtaStatus::ServerError;
  }
#else
  return _init_status;
#endif
}

int WifiHttpOtaSource::read(uint8_t *buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return -1;
  }

#ifndef TEST_HOST
  if (_client == nullptr) {
    return -1;
  }
  return esp_http_client_read(static_cast<esp_http_client_handle_t>(_client),
                              reinterpret_cast<char *>(buf), static_cast<int>(buf_size));
#else
  return -1;
#endif
}

void WifiHttpOtaSource::close() {
#ifndef TEST_HOST
  if (_client == nullptr) {
    return;
  }
  auto client = static_cast<esp_http_client_handle_t>(_client);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  _client = nullptr;
#endif
}
