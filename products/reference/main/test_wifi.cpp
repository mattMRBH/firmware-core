#include "test_wifi.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "drivers/esp_wifi_hal.h"
#include "rtos.h"
#include "services/wifi_manager.h"
#include "types/wifi_types.h"

// ---------------------------------------------------------------------------
// Build-time configuration. Override with -D in EXTRA_CXXFLAGS.
// ---------------------------------------------------------------------------

#ifndef TEST_WIFI_SSID
#define TEST_WIFI_SSID ""
#endif
#ifndef TEST_WIFI_PASSWORD
#define TEST_WIFI_PASSWORD ""
#endif

#ifndef TEST_WIFI_AP_SSID
#define TEST_WIFI_AP_SSID "agwifi-test-ap"
#endif
#ifndef TEST_WIFI_AP_PASSWORD
// Empty string => open AP. WPA2-PSK requires >= 8 characters.
#define TEST_WIFI_AP_PASSWORD ""
#endif

#ifndef TEST_WIFI_HOSTNAME
#define TEST_WIFI_HOSTNAME "agwifi-test"
#endif

#ifndef TEST_WIFI_STATUS_INTERVAL_MS
#define TEST_WIFI_STATUS_INTERVAL_MS 5000U
#endif

static constexpr const char *TAG = "test_wifi";

namespace {

const char *_mode_to_str(WifiMode mode) {
  switch (mode) {
  case WifiMode::Off:
    return "Off";
  case WifiMode::Sta:
    return "Sta";
  case WifiMode::Ap:
    return "Ap";
  case WifiMode::ApSta:
    return "ApSta";
  }
  return "?";
}

const char *_sta_state_to_str(WifiStaState state) {
  switch (state) {
  case WifiStaState::Disconnected:
    return "Disconnected";
  case WifiStaState::Connecting:
    return "Connecting";
  case WifiStaState::Connected:
    return "Connected";
  case WifiStaState::GotIp:
    return "GotIp";
  }
  return "?";
}

const char *_reason_to_str(WifiDisconnectReason reason) {
  switch (reason) {
  case WifiDisconnectReason::Unknown:
    return "Unknown";
  case WifiDisconnectReason::AuthFailed:
    return "AuthFailed";
  case WifiDisconnectReason::NoApFound:
    return "NoApFound";
  case WifiDisconnectReason::AssocFailed:
    return "AssocFailed";
  case WifiDisconnectReason::ApDisconnected:
    return "ApDisconnected";
  case WifiDisconnectReason::ConnectionLost:
    return "ConnectionLost";
  case WifiDisconnectReason::HandshakeFailed:
    return "HandshakeFailed";
  case WifiDisconnectReason::DhcpFailed:
    return "DhcpFailed";
  case WifiDisconnectReason::RequestedByUser:
    return "RequestedByUser";
  }
  return "?";
}

const char *_auth_to_str(WifiAuthMode mode) {
  switch (mode) {
  case WifiAuthMode::Open:
    return "OPEN";
  case WifiAuthMode::Wep:
    return "WEP";
  case WifiAuthMode::WpaPsk:
    return "WPA-PSK";
  case WifiAuthMode::Wpa2Psk:
    return "WPA2-PSK";
  case WifiAuthMode::WpaWpa2Psk:
    return "WPA/WPA2-PSK";
  case WifiAuthMode::Wpa3Psk:
    return "WPA3-PSK";
  case WifiAuthMode::Wpa2Wpa3Psk:
    return "WPA2/WPA3-PSK";
  case WifiAuthMode::WapiPsk:
    return "WAPI-PSK";
  case WifiAuthMode::Owe:
    return "OWE";
  case WifiAuthMode::Unknown:
  default:
    return "?";
  }
}

void _log_ip(uint32_t ip) {
  // ip is in network byte order (little-endian on ESP32 host).
  const uint8_t b0 = static_cast<uint8_t>(ip & 0xFF);
  const uint8_t b1 = static_cast<uint8_t>((ip >> 8) & 0xFF);
  const uint8_t b2 = static_cast<uint8_t>((ip >> 16) & 0xFF);
  const uint8_t b3 = static_cast<uint8_t>((ip >> 24) & 0xFF);
  ESP_LOGI(TAG, "got IP: %u.%u.%u.%u", b0, b1, b2, b3);
}

void _log_snapshot(const WifiStatusSnapshot &snap) {
  ESP_LOGI(TAG, "status: mode=%s sta=%s rssi=%d ch=%u ip=0x%08" PRIX32 " ap_clients=%u",
           _mode_to_str(snap.mode), _sta_state_to_str(snap.sta_state), snap.rssi, snap.channel,
           snap.ip, snap.ap_client_count);
}

#ifndef TEST_WIFI_RUN_AP

bool _wait_for_disconnected(const WifiManager &mgr, uint32_t timeout_ms) {
  const uint64_t deadline = RTOS::get_time_ms() + timeout_ms;
  while (RTOS::get_time_ms() < deadline) {
    if (mgr.status_snapshot().sta_state == WifiStaState::Disconnected) {
      return true;
    }
    RTOS::delay_ms(50);
  }
  return false;
}

void _run_sta_test(WifiManager &mgr) {
  static constexpr const char *kSsid = TEST_WIFI_SSID;
  static constexpr const char *kPassword = TEST_WIFI_PASSWORD;

  if (kSsid[0] == '\0') {
    ESP_LOGE(TAG, "TEST_WIFI_SSID is empty — pass it via "
                  "-DEXTRA_CXXFLAGS='-DTEST_WIFI_SSID=\\\"...\\\" "
                  "-DTEST_WIFI_PASSWORD=\\\"...\\\"'");
    return;
  }

  // Configure mDNS BEFORE connecting; the manager auto-starts mDNS on
  // got-IP and auto-stops on disconnect / mode-Off.
  WifiMdnsServiceRecord svc = {};
  svc.service_type = "_http._tcp";
  svc.port = 80;
  WifiMdnsConfig mdns = {};
  mdns.hostname = TEST_WIFI_HOSTNAME;
  mdns.services = &svc;
  mdns.service_count = 1;
  if (mgr.set_mdns_config(mdns) != WifiStatus::Ok) {
    ESP_LOGW(TAG, "set_mdns_config failed");
  }

  if (mgr.set_mode(WifiMode::Sta) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Sta) failed");
    return;
  }

  // Trigger a scan first — only legal while disconnected (per spec).
  ESP_LOGI(TAG, "starting scan...");
  if (mgr.start_scan() != WifiStatus::Ok) {
    ESP_LOGW(TAG, "start_scan failed");
  }
  // The scan callback prints results; give it a few seconds before connecting.
  if (!_wait_for_disconnected(mgr, 8000)) {
    ESP_LOGW(TAG, "scan still in progress after 8s — proceeding anyway");
  }
  RTOS::delay_ms(1000);

  WifiStaConfig cfg;
  std::strncpy(cfg.ssid, kSsid, sizeof(cfg.ssid) - 1);
  std::strncpy(cfg.password, kPassword, sizeof(cfg.password) - 1);
  cfg.max_retry_count = 5;
  cfg.initial_retry_interval_ms = 1000;
  cfg.max_retry_interval_ms = 16000;

  ESP_LOGI(TAG, "connecting to '%s'...", kSsid);
  const WifiStatus connect_status = mgr.connect(cfg);
  if (connect_status != WifiStatus::Ok) {
    ESP_LOGE(TAG, "connect() returned %d", static_cast<int>(connect_status));
    return;
  }

  while (true) {
    RTOS::delay_ms(TEST_WIFI_STATUS_INTERVAL_MS);
    _log_snapshot(mgr.status_snapshot());
  }
}

#else // TEST_WIFI_RUN_AP

void _run_ap_test(WifiManager &mgr) {
  static constexpr const char *kSsid = TEST_WIFI_AP_SSID;
  static constexpr const char *kPassword = TEST_WIFI_AP_PASSWORD;

  if (mgr.set_mode(WifiMode::Ap) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Ap) failed");
    return;
  }

  WifiApConfig ap = {};
  std::strncpy(ap.ssid, kSsid, sizeof(ap.ssid) - 1);
  std::strncpy(ap.password, kPassword, sizeof(ap.password) - 1);
  ap.channel = 1;
  ap.max_connections = 4;

  ESP_LOGI(TAG, "starting AP '%s' (auth=%s)", kSsid, kPassword[0] == '\0' ? "OPEN" : "WPA2-PSK");
  if (mgr.start_ap(ap) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "start_ap failed");
    return;
  }

  while (true) {
    RTOS::delay_ms(TEST_WIFI_STATUS_INTERVAL_MS);
    _log_snapshot(mgr.status_snapshot());
  }
}

#endif // TEST_WIFI_RUN_AP

} // namespace

void run_test_wifi() {
  ESP_LOGI(TAG, "--- Wi-Fi test start (runs indefinitely) ---");

  EspWifiHal hal;
  WifiManager mgr(hal);

  if (hal.init() != WifiStatus::Ok) {
    ESP_LOGE(TAG, "EspWifiHal::init failed");
    return;
  }

  mgr.set_on_connected([]() { ESP_LOGI(TAG, "event: connected (L2 link up)"); });
  mgr.set_on_got_ip([](uint32_t ip) { _log_ip(ip); });
  mgr.set_on_disconnected([](WifiDisconnectReason r) {
    ESP_LOGW(TAG, "event: disconnected reason=%s", _reason_to_str(r));
  });
  mgr.set_on_scan_complete([](const WifiScanEntry *results, uint16_t count) {
    ESP_LOGI(TAG, "scan complete: %u network%s", count, count == 1 ? "" : "s");
    for (uint16_t i = 0; i < count; ++i) {
      const WifiScanEntry &e = results[i];
      ESP_LOGI(TAG,
               "  [%u] ssid='%s' rssi=%d ch=%u auth=%s "
               "bssid=%02X:%02X:%02X:%02X:%02X:%02X",
               i, e.ssid, e.rssi, e.channel, _auth_to_str(e.auth_mode), e.bssid[0], e.bssid[1],
               e.bssid[2], e.bssid[3], e.bssid[4], e.bssid[5]);
    }
  });
  mgr.set_on_ap_client_joined([](const uint8_t mac[6]) {
    ESP_LOGI(TAG, "AP client joined: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  });
  mgr.set_on_ap_client_left([](const uint8_t mac[6]) {
    ESP_LOGI(TAG, "AP client left:   %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  });

#ifndef TEST_WIFI_RUN_AP
  _run_sta_test(mgr);
#else
  _run_ap_test(mgr);
#endif
}
