/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "provisioning_manager.h"

#include <cstdio>
#include <cstring>

#include "../internal/ble_transport.h"
#include "../internal/captive_dns_responder.h"
#include "../internal/provisioning_timer.h"
#include "../internal/wifi_portal_transport.h"
#include "ag_log.h"
#include "hal/ble_server.h"
#include "hal/http_server.h"
#include "services/wifi_manager.h"

namespace {

constexpr const char *TAG = "Provisioning";

// Default AP IP on the soft-AP gateway (192.168.4.1) in network byte
// order (octet 0 in the low byte). lwIP's softAP defaults to this.
constexpr uint32_t DEFAULT_AP_IP_BE = 0x0104a8c0; // 192.168.4.1

void format_mac(const uint8_t mac[6], char out[18]) {
  std::snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
}

void format_ipv4_be(uint32_t ip_be, char out[16]) {
  std::snprintf(out, 16, "%u.%u.%u.%u", static_cast<unsigned>(ip_be & 0xFF),
                static_cast<unsigned>((ip_be >> 8) & 0xFF),
                static_cast<unsigned>((ip_be >> 16) & 0xFF),
                static_cast<unsigned>((ip_be >> 24) & 0xFF));
}

} // namespace

// Portal HTML linker symbols (provided by EMBED_FILES in the firmware
// build). Under TEST_HOST these aren't available; the portal transport
// registers only API routes.
#ifndef TEST_HOST
extern "C" const uint8_t _binary_portal_html_start[] asm("_binary_portal_html_start");
extern "C" const uint8_t _binary_portal_html_end[] asm("_binary_portal_html_end");
#endif

ProvisioningManager::ProvisioningManager()
    : _portal(std::make_unique<WifiPortalTransport>()),
      _ble_transport(std::make_unique<BleTransport>()),
      _dns(std::make_unique<CaptiveDnsResponder>()), _timer(std::make_unique<ProvisioningTimer>()) {
  _timer->set_callback([this]() { _on_timeout(); });
}

ProvisioningManager::~ProvisioningManager() {
  // Clear the callback before the destructor's stop() to prevent firing
  // Stopped into potentially-destroyed captures. If the product wanted
  // the Stopped event, they should have called stop() explicitly before
  // the manager goes out of scope.
  _on_event = nullptr;
  stop();
}

void ProvisioningManager::set_on_event(ProvisioningEventCallback cb) {
  _mutex.lock();
  _on_event = std::move(cb);
  _mutex.unlock();
}

bool ProvisioningManager::start(WifiManager &wifi, AgBleServer &ble, HttpServer &http,
                                const ProvisioningConfig &config) {
  _mutex.lock();
  if (_state != ProvisioningState::Idle) {
    AG_LOGW(TAG, "start() called while not Idle (state=%u)", static_cast<unsigned>(_state));
    _mutex.unlock();
    return false;
  }
  if (config.ap.ssid[0] == '\0') {
    AG_LOGE(TAG, "AP SSID is required");
    _mutex.unlock();
    return false;
  }

  AG_LOGI(TAG,
          "start(): AP ssid='%s' ch=%u port=%u overall_timeout=%u ms connect_timeout=%u ms "
          "ble='%s' model='%s'",
          config.ap.ssid, static_cast<unsigned>(config.ap.channel),
          static_cast<unsigned>(config.http_port), static_cast<unsigned>(config.overall_timeout_ms),
          static_cast<unsigned>(config.connect_timeout_ms),
          config.ble.device_name != nullptr ? config.ble.device_name : "",
          config.ble.model_name != nullptr ? config.ble.model_name : "");

  _wifi = &wifi;
  _http = &http;
  _config = config;
  _ap_client_count = 0;
  _ble_client_count = 0;
  _timeout_armed = false;

  // -- Portal transport --
  _portal->set_on_credentials([this](const ProvisioningData &d) { return _accept_credentials(d); });
  _portal->set_on_scan_request([this]() { return _trigger_scan(); });
  _portal->set_state(WifiPortalTransport::PortalState::Waiting);

  const uint8_t *html_start = nullptr;
  const uint8_t *html_end = nullptr;
#ifndef TEST_HOST
  html_start = _binary_portal_html_start;
  html_end = _binary_portal_html_end;
#endif
  if (!_portal->register_routes(http, html_start, html_end)) {
    AG_LOGE(TAG, "portal route registration failed");
    http.unregister_all();
    _wifi = nullptr;
    _http = nullptr;
    _mutex.unlock();
    return false;
  }

  // -- BLE transport --
  _ble_transport->set_on_credentials(
      [this](const ProvisioningData &d) { return _accept_credentials(d); });
  _ble_transport->set_on_scan_request([this]() { return _trigger_scan(); });
  _ble_transport->set_on_client_connected([this]() { _on_ble_client_connected(); });
  _ble_transport->set_on_client_disconnected([this]() { _on_ble_client_disconnected(); });

  if (!_ble_transport->setup(ble, config.ble)) {
    AG_LOGE(TAG, "BLE transport setup failed");
    // Rollback portal routes.
    http.unregister_all();
    _wifi = nullptr;
    _http = nullptr;
    _mutex.unlock();
    return false;
  }

  // -- WiFi manager callbacks --
  wifi.set_on_scan_complete([this](const WifiScanEntry *r, uint16_t c) { _on_scan_results(r, c); });
  wifi.set_on_ap_client_joined([this](const uint8_t *mac) { _on_ap_client_joined(mac); });
  wifi.set_on_ap_client_left([this](const uint8_t *mac) { _on_ap_client_left(mac); });
  wifi.set_on_got_ip([this](uint32_t ip) { _on_sta_connected(ip); });
  wifi.set_on_disconnected([this](WifiDisconnectReason reason) {
    (void)reason;
    _on_sta_disconnected();
  });

  // Bound how long WifiManager will wait for DHCP after L2 association.
  if (_config.connect_timeout_ms > 0) {
    wifi.set_dhcp_timeout_ms(_config.connect_timeout_ms);
  }

  // Bring up Wi-Fi: ApSta mode + AP. STA connect happens later when
  // credentials are submitted. set_mode and start_ap are hard failures
  // — without them the soft-AP never comes up and the portal is
  // unreachable, so the caller must be told.
  if (wifi.set_mode(WifiMode::ApSta) != WifiStatus::Ok) {
    AG_LOGE(TAG, "set_mode(ApSta) failed");
    _rollback_start_locked(wifi, http);
    _mutex.unlock();
    return false;
  }

  WifiApConfig ap_cfg = {};
  std::strncpy(ap_cfg.ssid, _config.ap.ssid, sizeof(ap_cfg.ssid) - 1);
  std::strncpy(ap_cfg.password, _config.ap.password, sizeof(ap_cfg.password) - 1);
  ap_cfg.channel = _config.ap.channel;
  ap_cfg.max_connections = _config.ap.max_clients;
  if (wifi.start_ap(ap_cfg) != WifiStatus::Ok) {
    AG_LOGE(TAG, "start_ap failed");
    _rollback_start_locked(wifi, http);
    _mutex.unlock();
    return false;
  }

  // DNS responder powers captive-portal auto-popup; if it fails the
  // portal is still reachable manually at the AP IP, so this is a
  // degraded-but-usable case — log and continue. _dns->stop() is
  // idempotent so the later teardown paths don't need to know whether
  // start succeeded.
  if (!_dns->start(DEFAULT_AP_IP_BE)) {
    AG_LOGW(TAG, "captive DNS responder failed to start; portal still reachable manually");
  }

  // Start the HTTP server now that all portal routes are registered.
  if (!http.start(_config.http_port)) {
    AG_LOGE(TAG, "http.start(:%u) failed", static_cast<unsigned>(_config.http_port));
    _rollback_start_locked(wifi, http);
    _mutex.unlock();
    return false;
  }

  _set_state_locked(ProvisioningState::WaitingForCredentials);
  _maybe_arm_timeout_locked();

  AG_LOGI(TAG, "provisioning started: AP='%s' ch=%u port=%u", _config.ap.ssid,
          static_cast<unsigned>(_config.ap.channel), static_cast<unsigned>(_config.http_port));

  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Started;
  _mutex.unlock();
  _emit(info);
  return true;
}

void ProvisioningManager::stop(bool stop_http_server) {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::ProductRequested;

  _mutex.lock();
  if (_state == ProvisioningState::Idle) {
    _mutex.unlock();
    return;
  }
  AG_LOGI(TAG, "stop() requested (state=%u stop_http_server=%d)", static_cast<unsigned>(_state),
          stop_http_server ? 1 : 0);
  _timer->cancel();
  _timeout_armed = false;

  _dns->stop();

  // Clear BLE transport callbacks before teardown so the disconnect
  // triggered by deinit() doesn't call back into the manager while
  // the mutex is held (which would deadlock).
  _ble_transport->set_on_credentials(nullptr);
  _ble_transport->set_on_scan_request(nullptr);
  _ble_transport->set_on_client_connected(nullptr);
  _ble_transport->set_on_client_disconnected(nullptr);
  _ble_transport->teardown();

  if (_http != nullptr) {
    _http->unregister_all();
    if (stop_http_server) {
      _http->stop();
    }
  }

  // Detach WifiManager callbacks before mode change.
  if (_wifi != nullptr) {
    _wifi->set_on_scan_complete(nullptr);
    _wifi->set_on_ap_client_joined(nullptr);
    _wifi->set_on_ap_client_left(nullptr);
    _wifi->set_on_got_ip(nullptr);
    _wifi->set_on_disconnected(nullptr);
    _wifi->set_mode(WifiMode::Sta);
  }
  _wifi = nullptr;
  _http = nullptr;
  _set_state_locked(ProvisioningState::Idle);
  _mutex.unlock();

  AG_LOGI(TAG, "provisioning stopped (reason=%u)", static_cast<unsigned>(info.stop_reason));
  _emit(info);
}

ProvisioningState ProvisioningManager::state() const {
  _mutex.lock();
  ProvisioningState s = _state;
  _mutex.unlock();
  return s;
}

void ProvisioningManager::send_ble_status(uint8_t status_code) {
  _mutex.lock();
  if (_state == ProvisioningState::Idle) {
    _mutex.unlock();
    return;
  }
  AG_LOGD(TAG, "send_ble_status code=%u", static_cast<unsigned>(status_code));
  _ble_transport->send_status(status_code);
  _mutex.unlock();
}

// ---------------------------------------------------------------------------
// Private event handlers
// ---------------------------------------------------------------------------

void ProvisioningManager::_on_scan_results(const WifiScanEntry *entries, uint16_t count) {
  AG_LOGI(TAG, "scan complete: %u raw entries", static_cast<unsigned>(count));
  _mutex.lock();
  _portal->update_scan_results(entries, count);
  _ble_transport->update_scan_results(entries, count);
  _mutex.unlock();
}

void ProvisioningManager::_on_sta_connected(uint32_t ip) {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Connected;

  _mutex.lock();
  if (_state != ProvisioningState::Connecting) {
    AG_LOGD(TAG, "ignored stale STA connected in state=%u", static_cast<unsigned>(_state));
    _mutex.unlock();
    return;
  }
  info.data = _pending_data;
  info.ip = ip;
  _portal->set_state(WifiPortalTransport::PortalState::Connected);
  _ble_transport->send_status(ProvisioningBleStatus::WIFI_CONNECTED);
  _set_state_locked(ProvisioningState::Connected);
  _pause_timeout_locked(); // no further timeout in Connected
  _mutex.unlock();
  _emit(info);
}

void ProvisioningManager::_on_sta_disconnected() {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::ConnectFailed;

  _mutex.lock();
  if (_state != ProvisioningState::Connecting) {
    AG_LOGD(TAG, "ignored stale STA disconnect in state=%u", static_cast<unsigned>(_state));
    _mutex.unlock();
    return;
  }
  AG_LOGI(TAG, "STA connect failed for ssid='%s'", _pending_data.ssid);
  _portal->set_state(WifiPortalTransport::PortalState::Failed);
  _ble_transport->send_status(ProvisioningBleStatus::WIFI_CONNECT_FAILED);
  _set_state_locked(ProvisioningState::WaitingForCredentials);
  _maybe_arm_timeout_locked();
  _mutex.unlock();
  _emit(info);
}

void ProvisioningManager::_on_ap_client_joined(const uint8_t *mac) {
  _mutex.lock();
  ++_ap_client_count;
  if (mac != nullptr) {
    char mac_str[18];
    format_mac(mac, mac_str);
    AG_LOGI(TAG, "AP client joined: %s (ap=%u ble=%u)", mac_str,
            static_cast<unsigned>(_ap_client_count), static_cast<unsigned>(_ble_client_count));
  } else {
    AG_LOGI(TAG, "AP client joined (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
            static_cast<unsigned>(_ble_client_count));
  }
  if (_total_client_count_locked() == 1) {
    _pause_timeout_locked();
  }
  _mutex.unlock();
}

void ProvisioningManager::_on_ap_client_left(const uint8_t *mac) {
  _mutex.lock();
  if (_ap_client_count > 0) {
    --_ap_client_count;
  }
  if (mac != nullptr) {
    char mac_str[18];
    format_mac(mac, mac_str);
    AG_LOGI(TAG, "AP client left: %s (ap=%u ble=%u)", mac_str,
            static_cast<unsigned>(_ap_client_count), static_cast<unsigned>(_ble_client_count));
  } else {
    AG_LOGI(TAG, "AP client left (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
            static_cast<unsigned>(_ble_client_count));
  }
  if (_total_client_count_locked() == 0) {
    _resume_timeout_locked();
  }
  _mutex.unlock();
}

void ProvisioningManager::_on_ble_client_connected() {
  _mutex.lock();
  ++_ble_client_count;
  AG_LOGI(TAG, "BLE client connected (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
          static_cast<unsigned>(_ble_client_count));
  if (_total_client_count_locked() == 1) {
    _pause_timeout_locked();
  }
  _mutex.unlock();
}

void ProvisioningManager::_on_ble_client_disconnected() {
  _mutex.lock();
  if (_ble_client_count > 0) {
    --_ble_client_count;
  }
  AG_LOGI(TAG, "BLE client disconnected (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
          static_cast<unsigned>(_ble_client_count));
  if (_total_client_count_locked() == 0) {
    _resume_timeout_locked();
  }
  _mutex.unlock();
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

void ProvisioningManager::_set_state_locked(ProvisioningState s) {
  if (s != _state) {
    AG_LOGI(TAG, "state: %u -> %u", static_cast<unsigned>(_state), static_cast<unsigned>(s));
  }
  _state = s;
}

void ProvisioningManager::_emit(const ProvisioningEventInfo &info) {
  ProvisioningEventCallback cb;
  _mutex.lock();
  cb = _on_event;
  _mutex.unlock();
  if (cb) {
    cb(info);
  }
}

bool ProvisioningManager::_accept_credentials(const ProvisioningData &data) {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Connecting;

  _mutex.lock();
  if (_state != ProvisioningState::WaitingForCredentials) {
    AG_LOGW(TAG, "credentials rejected — state=%u (expected WaitingForCredentials)",
            static_cast<unsigned>(_state));
    _mutex.unlock();
    return false;
  }
  AG_LOGI(TAG, "credentials accepted: ssid='%s' pw_len=%u disable_cloud=%d static_ip=%d", data.ssid,
          static_cast<unsigned>(std::strlen(data.password)), data.disable_cloud ? 1 : 0,
          data.has_static_ip() ? 1 : 0);
  _pending_data = data;
  info.data = data;
  _portal->set_state(WifiPortalTransport::PortalState::Connecting);
  _set_state_locked(ProvisioningState::Connecting);
  _pause_timeout_locked();

  if (_wifi != nullptr) {
    if (data.has_static_ip()) {
      char ip_str[16];
      char nm_str[16];
      char gw_str[16];
      char dns_str[16];
      format_ipv4_be(data.static_ip.ip, ip_str);
      format_ipv4_be(data.static_ip.netmask, nm_str);
      format_ipv4_be(data.static_ip.gateway, gw_str);
      format_ipv4_be(data.static_ip.dns_primary, dns_str);
      AG_LOGI(TAG, "applying static IP %s/%s gw=%s dns=%s", ip_str, nm_str, gw_str, dns_str);
      _wifi->set_static_ip(data.static_ip);
    } else {
      AG_LOGI(TAG, "no static IP — using DHCP");
      _wifi->clear_static_ip();
    }
    WifiStaConfig sta = {};
    std::strncpy(sta.ssid, data.ssid, sizeof(sta.ssid) - 1);
    std::strncpy(sta.password, data.password, sizeof(sta.password) - 1);
    sta.max_retry_count = 0;
    _wifi->connect(sta);
  }
  _mutex.unlock();
  _emit(info);
  return true;
}

bool ProvisioningManager::_trigger_scan() {
  _mutex.lock();
  if (_state != ProvisioningState::WaitingForCredentials) {
    AG_LOGD(TAG, "scan ignored in state=%u", static_cast<unsigned>(_state));
    _mutex.unlock();
    return false;
  }
  AG_LOGI(TAG, "wifi scan triggered");
  if (_wifi != nullptr) {
    _wifi->start_scan({});
  }
  _mutex.unlock();
  return true;
}

void ProvisioningManager::_on_timeout() {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::TimedOut;

  _mutex.lock();
  if (_state == ProvisioningState::Idle) {
    _mutex.unlock();
    return;
  }
  if (_total_client_count_locked() > 0) {
    // Spurious — clients reattached after the timer fired. Re-arm.
    AG_LOGW(TAG, "timeout fired but clients present (ap=%u ble=%u) — re-arming",
            static_cast<unsigned>(_ap_client_count), static_cast<unsigned>(_ble_client_count));
    _maybe_arm_timeout_locked();
    _mutex.unlock();
    return;
  }
  AG_LOGI(TAG, "inactivity timeout expired — tearing down");
  _timeout_armed = false;
  _dns->stop();

  _ble_transport->set_on_credentials(nullptr);
  _ble_transport->set_on_scan_request(nullptr);
  _ble_transport->set_on_client_connected(nullptr);
  _ble_transport->set_on_client_disconnected(nullptr);
  _ble_transport->teardown();

  if (_http != nullptr) {
    _http->unregister_all();
    _http->stop();
  }
  if (_wifi != nullptr) {
    _wifi->set_on_scan_complete(nullptr);
    _wifi->set_on_ap_client_joined(nullptr);
    _wifi->set_on_ap_client_left(nullptr);
    _wifi->set_on_got_ip(nullptr);
    _wifi->set_on_disconnected(nullptr);
    _wifi->set_mode(WifiMode::Sta);
  }
  _wifi = nullptr;
  _http = nullptr;
  _set_state_locked(ProvisioningState::Idle);
  _mutex.unlock();
  AG_LOGI(TAG, "provisioning stopped (reason=%u)", static_cast<unsigned>(info.stop_reason));
  _emit(info);
}

void ProvisioningManager::_maybe_arm_timeout_locked() {
  if (_config.overall_timeout_ms == 0) {
    return;
  }
  if (_state != ProvisioningState::WaitingForCredentials) {
    return;
  }
  if (_total_client_count_locked() > 0) {
    return;
  }
  _timer->arm(_config.overall_timeout_ms);
  _timeout_armed = true;
  AG_LOGI(TAG, "inactivity timeout armed (%u ms)",
          static_cast<unsigned>(_config.overall_timeout_ms));
}

void ProvisioningManager::_pause_timeout_locked() {
  if (_timeout_armed) {
    _timer->cancel();
    _timeout_armed = false;
    AG_LOGD(TAG, "inactivity timeout paused");
  }
}

void ProvisioningManager::_resume_timeout_locked() {
  if (_config.overall_timeout_ms == 0) {
    return;
  }
  if (_state != ProvisioningState::WaitingForCredentials) {
    return;
  }
  if (!_timeout_armed) {
    _timer->arm(_config.overall_timeout_ms);
    _timeout_armed = true;
    AG_LOGD(TAG, "inactivity timeout resumed (%u ms)",
            static_cast<unsigned>(_config.overall_timeout_ms));
  }
}

uint32_t ProvisioningManager::_total_client_count_locked() const {
  return _ap_client_count + _ble_client_count;
}

void ProvisioningManager::_rollback_start_locked(WifiManager &wifi, HttpServer &http) {
  // Clear BLE callbacks before teardown so a disconnect triggered by
  // deinit() doesn't re-enter the manager while the mutex is held.
  _ble_transport->set_on_credentials(nullptr);
  _ble_transport->set_on_scan_request(nullptr);
  _ble_transport->set_on_client_connected(nullptr);
  _ble_transport->set_on_client_disconnected(nullptr);
  _ble_transport->teardown();

  _dns->stop(); // idempotent — safe whether start succeeded or not

  // Detach WifiManager callbacks before mode change (mirrors stop()).
  wifi.set_on_scan_complete(nullptr);
  wifi.set_on_ap_client_joined(nullptr);
  wifi.set_on_ap_client_left(nullptr);
  wifi.set_on_got_ip(nullptr);
  wifi.set_on_disconnected(nullptr);
  wifi.set_mode(WifiMode::Sta);

  // start() only calls http.start() at its final step; if we got here
  // the HTTP server is either not started or about to fail-start, so
  // unregistering routes is enough — no http.stop() needed.
  http.unregister_all();

  _wifi = nullptr;
  _http = nullptr;
}
