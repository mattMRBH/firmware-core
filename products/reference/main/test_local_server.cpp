#include "test_local_server.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/esp_wifi_hal.h"
#include "drivers/idf_http_server.h"
#include "hal/action_handler.h"
#include "hal/config_provider.h"
#include "hal/measures_provider.h"
#include "measures_types.h"
#include "nvs_config_store.h"
#include "rtos.h"
#include "services/local_server.h"
#include "services/wifi_manager.h"
#include "types/local_config.h"
#include "types/local_server_result.h"
#include "types/system_info.h"
#include "types/wifi_types.h"

// ---------------------------------------------------------------------------
// Build-time Wi-Fi credentials. Override with -D in EXTRA_CXXFLAGS.
// ---------------------------------------------------------------------------
#ifndef TEST_LOCAL_SERVER_WIFI_SSID
#define TEST_LOCAL_SERVER_WIFI_SSID "airgradient"
#endif
#ifndef TEST_LOCAL_SERVER_WIFI_PASSWORD
#define TEST_LOCAL_SERVER_WIFI_PASSWORD "cleanair"
#endif

static constexpr const char *TAG = "test_local_server";

namespace {

constexpr uint8_t CONNECT_MAX_RETRY = 5;
constexpr uint32_t CONNECT_INITIAL_RETRY_MS = 1000;
constexpr uint32_t CONNECT_MAX_RETRY_MS = 16000;
constexpr uint32_t STATUS_INTERVAL_MS = 10000;

// Semantic validation ranges (the component only does type / enum checks;
// range validation belongs to the product apply path).
constexpr int CO2_CALIB_DAYS_MAX = 30;
constexpr int LEARNING_OFFSET_MAX = 720;
constexpr int BRIGHTNESS_MAX = 100;
constexpr size_t COUNTRY_CODE_LEN = 2;

void log_ip(uint32_t ip) {
  // ip is in network byte order (octet 0 in the low byte).
  ESP_LOGI(TAG, "v1 API reachable at http://%u.%u.%u.%u/api/v1/measures",
           static_cast<unsigned>(ip & 0xFF), static_cast<unsigned>((ip >> 8) & 0xFF),
           static_cast<unsigned>((ip >> 16) & 0xFF), static_cast<unsigned>((ip >> 24) & 0xFF));
}

bool connect_sta(WifiManager &mgr) {
  if (TEST_LOCAL_SERVER_WIFI_SSID[0] == '\0') {
    ESP_LOGE(TAG, "TEST_LOCAL_SERVER_WIFI_SSID is empty — pass it via EXTRA_CXXFLAGS");
    return false;
  }
  if (mgr.set_mode(WifiMode::Sta) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Sta) failed");
    return false;
  }

  WifiStaConfig cfg = {};
  std::strncpy(cfg.ssid, TEST_LOCAL_SERVER_WIFI_SSID, sizeof(cfg.ssid) - 1);
  std::strncpy(cfg.password, TEST_LOCAL_SERVER_WIFI_PASSWORD, sizeof(cfg.password) - 1);
  cfg.max_retry_count = CONNECT_MAX_RETRY;
  cfg.initial_retry_interval_ms = CONNECT_INITIAL_RETRY_MS;
  cfg.max_retry_interval_ms = CONNECT_MAX_RETRY_MS;

  ESP_LOGI(TAG, "connecting to '%s'...", TEST_LOCAL_SERVER_WIFI_SSID);
  const WifiStatus st = mgr.connect(cfg);
  if (st != WifiStatus::Ok) {
    ESP_LOGE(TAG, "connect() returned %d", static_cast<int>(st));
    return false;
  }
  return true;
}

// --- Demo providers ------------------------------------------------------

// Synthetic readings plus a live wifi_rssi sourced from the station link.
// pm_b / electrode / power stay at invalid sentinels and are omitted.
class DemoMeasuresProvider : public MeasuresProvider {
public:
  explicit DemoMeasuresProvider(WifiManager &wifi) : _wifi(wifi) {}

  Measures get_measures() override {
    Measures m;
    m.co2.co2 = 612;
    m.pm_a.pm_01 = 5.0f;
    m.pm_a.pm_25 = 8.0f;
    m.pm_a.pm_10 = 9.0f;
    m.pm_a.pm_03_pc = 320.0f;
    m.temp_hum_a.temperature = 24.3f;
    m.temp_hum_a.humidity = 47.1f;
    m.tvoc_nox.tvoc_index = 101;
    m.tvoc_nox.nox_index = 1;
    return m;
  }

  SystemInfo get_system_info() override {
    SystemInfo info;
    std::strncpy(info.serial_number, "aabbccddeeff", sizeof(info.serial_number) - 1);
    std::strncpy(info.model, "O-1PST", sizeof(info.model) - 1);
    std::strncpy(info.firmware, "2.0.0", sizeof(info.firmware) - 1);
    // Report the real station RSSI once the link is up; otherwise leave it
    // unset so the key is omitted from the payload.
    const WifiStatusSnapshot snap = _wifi.status_snapshot();
    if (snap.sta_state == WifiStaState::Connected || snap.sta_state == WifiStaState::GotIp) {
      info.wifi_rssi = snap.rssi;
    }
    return info;
  }

private:
  WifiManager &_wifi;
};

// In-memory config. Validates every present field before applying anything
// (all-or-nothing) and merges the accepted partial into the stored config.
class DemoConfigProvider : public ConfigProvider {
public:
  DemoConfigProvider() {
    _cfg.country = "US";
    _cfg.pm_standard = "us-aqi";
    _cfg.temp_unit = "c";
    _cfg.cloud_enabled = true;
    _cfg.configuration_control = "both";
    _cfg.co2_calib_days = 8;
    _cfg.led_bar_mode = "co2";
    _cfg.led_bar_brightness = 80;
    _cfg.display_brightness = 90;
  }

  LocalServerConfig get_config() override { return _cfg; }

  ConfigApplyResult apply_config(const LocalServerConfig &p) override {
    // 1) Validate ALL present fields first.
    if (p.country.has_value() && p.country->size() != COUNTRY_CODE_LEN) {
      return {ConfigApplyStatus::InvalidValue, ConfigFieldId::CountryCode};
    }
    if (p.co2_calib_days.has_value() &&
        (*p.co2_calib_days < 0 || *p.co2_calib_days > CO2_CALIB_DAYS_MAX)) {
      return {ConfigApplyStatus::InvalidValue, ConfigFieldId::Co2CalibDays};
    }
    if (p.tvoc_offset.has_value() && (*p.tvoc_offset < 0 || *p.tvoc_offset > LEARNING_OFFSET_MAX)) {
      return {ConfigApplyStatus::InvalidValue, ConfigFieldId::TvocOffset};
    }
    if (p.nox_offset.has_value() && (*p.nox_offset < 0 || *p.nox_offset > LEARNING_OFFSET_MAX)) {
      return {ConfigApplyStatus::InvalidValue, ConfigFieldId::NoxOffset};
    }
    if (p.led_bar_brightness.has_value() &&
        (*p.led_bar_brightness < 0 || *p.led_bar_brightness > BRIGHTNESS_MAX)) {
      return {ConfigApplyStatus::InvalidValue, ConfigFieldId::LedBarBrightness};
    }
    if (p.display_brightness.has_value() &&
        (*p.display_brightness < 0 || *p.display_brightness > BRIGHTNESS_MAX)) {
      return {ConfigApplyStatus::InvalidValue, ConfigFieldId::DisplayBrightness};
    }

    // 2) All valid -> merge the present fields into the stored config.
    if (p.country.has_value()) {
      _cfg.country = p.country;
    }
    if (p.pm_standard.has_value()) {
      _cfg.pm_standard = p.pm_standard;
    }
    if (p.temp_unit.has_value()) {
      _cfg.temp_unit = p.temp_unit;
    }
    if (p.cloud_enabled.has_value()) {
      _cfg.cloud_enabled = p.cloud_enabled;
    }
    if (p.configuration_control.has_value()) {
      _cfg.configuration_control = p.configuration_control;
    }
    if (p.co2_calib_days.has_value()) {
      _cfg.co2_calib_days = p.co2_calib_days;
    }
    if (p.tvoc_offset.has_value()) {
      _cfg.tvoc_offset = p.tvoc_offset;
    }
    if (p.nox_offset.has_value()) {
      _cfg.nox_offset = p.nox_offset;
    }
    if (p.led_bar_mode.has_value()) {
      _cfg.led_bar_mode = p.led_bar_mode;
    }
    if (p.led_bar_brightness.has_value()) {
      _cfg.led_bar_brightness = p.led_bar_brightness;
    }
    if (p.display_brightness.has_value()) {
      _cfg.display_brightness = p.display_brightness;
    }

    ESP_LOGI(TAG, "config applied");
    return {ConfigApplyStatus::Ok, ConfigFieldId::None};
  }

private:
  LocalServerConfig _cfg;
};

// Logging action dispatcher. Real products would queue work on a worker;
// here we just log and report Dispatched (fire-and-forget).
class DemoActionHandler : public ActionHandler {
public:
  ActionResult trigger(ActionId action) override {
    switch (action) {
    case ActionId::CalibrateCo2:
      ESP_LOGI(TAG, "action: calibrate_co2 dispatched");
      break;
    case ActionId::TestLeds:
      ESP_LOGI(TAG, "action: test_leds dispatched");
      break;
    }
    return {ActionStatus::Dispatched};
  }
};

} // namespace

void run_test_local_server() {
  ESP_LOGI(TAG, "--- local server test start ---");

  // The wifi stack outlives this function trivially because
  // run_test_local_server never returns.
  EspWifiHal wifi_hal;
  NvsConfigStore wifi_creds(WIFI_CREDS_NVS_NAMESPACE);
  WifiManager wifi(wifi_hal, wifi_creds);
  if (wifi_hal.init() != WifiStatus::Ok) {
    ESP_LOGE(TAG, "EspWifiHal::init failed; aborting test");
    return;
  }

  wifi.set_on_connected([]() { ESP_LOGI(TAG, "event: connected (L2 link up)"); });
  wifi.set_on_got_ip([](uint32_t ip) { log_ip(ip); });
  wifi.set_on_disconnected([](WifiDisconnectReason r) {
    ESP_LOGW(TAG, "event: disconnected reason=%d", static_cast<int>(r));
  });

  if (!connect_sta(wifi)) {
    ESP_LOGE(TAG, "STA connect failed; aborting test");
    return;
  }

  // Everything below is heap-allocated so it lives for the whole program
  // (never freed; the test loops forever). LocalServer carries a multi-KB
  // scratch buffer, so keeping it off the app_main stack is intentional.
  auto *server = new IdfHttpServer();
  if (!server->start(CONFIG_AG_HTTP_PORT)) {
    ESP_LOGE(TAG, "server start failed");
    return;
  }
  ESP_LOGI(TAG, "HTTP server listening on port %d", CONFIG_AG_HTTP_PORT);

  auto *measures = new DemoMeasuresProvider(wifi);
  auto *config = new DemoConfigProvider();
  auto *actions = new DemoActionHandler();
  auto *local = new LocalServer(*server, {*measures, config, ConfigAccess::ReadWrite, actions});
  if (!local->begin()) {
    ESP_LOGE(TAG, "LocalServer::begin failed; aborting test");
    return;
  }
  ESP_LOGI(TAG, "v1 API registered: /api/v1/measures, /api/v1/config, /api/v1/actions/*");

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    const WifiStatusSnapshot snap = wifi.status_snapshot();
    ESP_LOGI(TAG, "still serving (sta_state=%d rssi=%d)", static_cast<int>(snap.sta_state),
             snap.rssi);
  }
}
