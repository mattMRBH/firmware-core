#include "test_http_server.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/idf_http_server.h"
#include "hal/http_request.h"
#include "hal/http_response.h"
#include "types/http_types.h"

// Embedded index.html shipped with this product (see main/CMakeLists.txt
// EMBED_FILES).  The two symbols below are produced by the linker.
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static constexpr const char *TAG = "test_http_server";

namespace {

constexpr const char *AP_SSID = "airgradient-ref";
constexpr const char *AP_PASSWORD = "airgradient";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNS = 4;

void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base != WIFI_EVENT) {
    return;
  }
  switch (id) {
  case WIFI_EVENT_AP_STACONNECTED: {
    const auto *evt = static_cast<const wifi_event_ap_staconnected_t *>(data);
    ESP_LOGI(TAG, "station " MACSTR " joined (aid=%d)", MAC2STR(evt->mac), evt->aid);
    break;
  }
  case WIFI_EVENT_AP_STADISCONNECTED: {
    const auto *evt = static_cast<const wifi_event_ap_stadisconnected_t *>(data);
    ESP_LOGI(TAG, "station " MACSTR " left (aid=%d)", MAC2STR(evt->mac), evt->aid);
    break;
  }
  default:
    break;
  }
}

bool init_softap() {
  if (esp_netif_init() != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init failed");
    return false;
  }

  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
    return false;
  }

  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init failed");
    return false;
  }

  if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler,
                                          nullptr, nullptr) != ESP_OK) {
    ESP_LOGE(TAG, "esp_event_handler_instance_register failed");
    return false;
  }

  wifi_config_t wifi_config = {};
  std::strncpy(reinterpret_cast<char *>(wifi_config.ap.ssid), AP_SSID, sizeof(wifi_config.ap.ssid));
  wifi_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(AP_SSID));
  std::strncpy(reinterpret_cast<char *>(wifi_config.ap.password), AP_PASSWORD,
               sizeof(wifi_config.ap.password));
  wifi_config.ap.channel = AP_CHANNEL;
  wifi_config.ap.max_connection = AP_MAX_CONNS;
  wifi_config.ap.authmode = std::strlen(AP_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
  wifi_config.ap.pmf_cfg.required = false;

  if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
      esp_wifi_set_config(WIFI_IF_AP, &wifi_config) != ESP_OK || esp_wifi_start() != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi setup failed");
    return false;
  }

  ESP_LOGI(TAG, "SoftAP up: ssid=\"%s\" channel=%u pw=\"%s\"", AP_SSID,
           static_cast<unsigned>(AP_CHANNEL), AP_PASSWORD);
  ESP_LOGI(TAG, "join the AP, then browse to http://192.168.4.1/");
  return true;
}

// --- Handlers ------------------------------------------------------------

void handle_status(const HttpRequest &, HttpResponse &resp) {
  const int64_t uptime_us = esp_timer_get_time();
  char buf[96];
  const int n =
      std::snprintf(buf, sizeof(buf), R"({"uptime_ms":%lld,"ssid":"%s","build":"reference"})",
                    static_cast<long long>(uptime_us / 1000), AP_SSID);
  if (n <= 0) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"snprintf"})");
    return;
  }
  resp.json(HttpStatus::Ok, buf, static_cast<size_t>(n));
}

void handle_echo(const HttpRequest &req, HttpResponse &resp) {
  if (req.body() == nullptr || req.body_length() == 0) {
    resp.json(HttpStatus::BadRequest, R"({"error":"empty body"})");
    return;
  }
  // Content type is whatever the client sent; we just mirror the bytes
  // as application/octet-stream to keep the demo simple.
  resp.body(HttpStatus::Ok, req.body(), req.body_length(), "application/octet-stream");
}

void handle_greet(const HttpRequest &req, HttpResponse &resp) {
  char name[32] = {};
  if (!req.get_query_param("name", name, sizeof(name))) {
    resp.json(HttpStatus::BadRequest, R"({"error":"missing name"})");
    return;
  }
  char buf[96];
  const int n = std::snprintf(buf, sizeof(buf), R"({"hello":"%s"})", name);
  if (n <= 0) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"snprintf"})");
    return;
  }
  resp.json(HttpStatus::Ok, buf, static_cast<size_t>(n));
}

} // namespace

void run_test_http_server() {
  ESP_LOGI(TAG, "--- HTTP server test start ---");

  if (!init_softap()) {
    ESP_LOGE(TAG, "SoftAP init failed; aborting test");
    return;
  }

  // Construct on the heap so it lives for the whole program. The driver
  // tears down the httpd in its destructor; we never call it because the
  // test loops forever.
  auto *server = new IdfHttpServer();

  bool ok = true;
  ok &= server->register_route(HttpMethod::Get, "/api/status", handle_status);
  ok &= server->register_route(HttpMethod::Post, "/api/echo", handle_echo);
  ok &= server->register_route(HttpMethod::Get, "/api/greet", handle_greet);
  ok &= server->register_static("/", index_html_start, index_html_end, "text/html");
  ok &= server->register_static("/index.html", index_html_start, index_html_end, "text/html");

  if (!ok) {
    ESP_LOGE(TAG, "route registration failed");
    return;
  }

  if (!server->start(CONFIG_AG_HTTP_PORT)) {
    ESP_LOGE(TAG, "server start failed");
    return;
  }

  ESP_LOGI(TAG, "HTTP server listening on port %d", CONFIG_AG_HTTP_PORT);

  // Keep this task alive so the server stays up.
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "still serving (uptime %lld ms)",
             static_cast<long long>(esp_timer_get_time() / 1000));
  }
}
