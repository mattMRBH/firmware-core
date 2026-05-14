/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "provisioning_manager.h"

#include <cstring>

#include "../internal/captive_dns_responder.h"
#include "../internal/provisioning_timer.h"
#include "../internal/wifi_portal_transport.h"
#include "ag_log.h"
#include "hal/http_server.h"
#include "services/wifi_manager.h"

namespace {

constexpr const char *TAG = "Provisioning";

// Default AP IP on the soft-AP gateway (192.168.4.1) in network byte
// order (octet 0 in the low byte). lwIP's softAP defaults to this.
constexpr uint32_t DEFAULT_AP_IP_BE = 0x0104a8c0; // 192.168.4.1

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

bool ProvisioningManager::start(WifiManager &wifi, AgBleServer *ble, HttpServer &http,
                                const ProvisioningConfig &config) {
  _mutex.lock();
  if (_state != ProvisioningState::Idle) {
    AG_LOGW(TAG, "start() called while not Idle");
    _mutex.unlock();
    return false;
  }
  if (config.ap.ssid[0] == '\0') {
    AG_LOGE(TAG, "AP SSID is required");
    _mutex.unlock();
    return false;
  }

  _wifi = &wifi;
  _ble = ble; // ignored in checkpoint 1
  _http = &http;
  _config = config;
  _ap_client_count = 0;
  _timeout_armed = false;

  // Wire portal callbacks. We hop out of the mutex inside the
  // callbacks below — accept_credentials and trigger_scan reacquire it.
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
    _wifi = nullptr;
    _http = nullptr;
    _mutex.unlock();
    return false;
  }

  // Wire WifiManager callbacks to private event handlers.
  wifi.set_on_scan_complete([this](const WifiScanEntry *r, uint16_t c) { _on_scan_results(r, c); });
  wifi.set_on_ap_client_joined([this](const uint8_t * /*mac*/) { _on_ap_client_joined(); });
  wifi.set_on_ap_client_left([this](const uint8_t * /*mac*/) { _on_ap_client_left(); });
  wifi.set_on_got_ip([this](uint32_t ip) { _on_sta_connected(ip); });
  wifi.set_on_disconnected([this](WifiDisconnectReason /*reason*/) { _on_sta_disconnected(); });

  // Bound how long WifiManager will wait for DHCP after L2 association.
  // ESP-IDF does not surface DHCP failure on its own — WifiManager arms
  // a watchdog and fires on_disconnected(dhcp_failed) on expiry, which
  // we forward as ConnectFailed. The default WIFI_DEFAULT_DHCP_TIMEOUT_MS
  // is fine; products can override via ProvisioningConfig.
  if (_config.connect_timeout_ms > 0) {
    wifi.set_dhcp_timeout_ms(_config.connect_timeout_ms);
  }

  // Bring up Wi-Fi: ApSta mode + AP. STA connect happens later when
  // credentials are submitted.
  if (wifi.set_mode(WifiMode::ApSta) != WifiStatus::Ok) {
    AG_LOGE(TAG, "set_mode(ApSta) failed");
  }

  WifiApConfig ap_cfg = {};
  std::strncpy(ap_cfg.ssid, _config.ap.ssid, sizeof(ap_cfg.ssid) - 1);
  std::strncpy(ap_cfg.password, _config.ap.password, sizeof(ap_cfg.password) - 1);
  ap_cfg.channel = _config.ap.channel;
  ap_cfg.max_connections = _config.ap.max_clients;
  if (wifi.start_ap(ap_cfg) != WifiStatus::Ok) {
    AG_LOGE(TAG, "start_ap failed");
  }

  _dns->start(DEFAULT_AP_IP_BE);

  // Start the HTTP server now that all portal routes are registered.
  // Contract: the caller hands us a fresh, not-yet-started server.
  if (!http.start(_config.http_port)) {
    AG_LOGE(TAG, "http.start(:%u) failed", static_cast<unsigned>(_config.http_port));
    // Best-effort rollback so the manager doesn't leave half-running
    // state. Drop the AP, stop DNS, clear the routes we just added.
    http.unregister_all();
    _dns->stop();
    wifi.set_mode(WifiMode::Sta);
    _wifi = nullptr;
    _http = nullptr;
    _mutex.unlock();
    return false;
  }

  _set_state_locked(ProvisioningState::WaitingForCredentials);
  _maybe_arm_timeout_locked();

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
  _timer->cancel();
  _timeout_armed = false;

  _dns->stop();

  // Wipe every HTTP route we registered, optionally stop the server.
  // The product accepts (contract) that the server is fully owned by
  // the manager for the duration of the session — anything registered
  // before start() would also go here, but the contract forbids that.
  if (_http != nullptr) {
    _http->unregister_all();
    if (stop_http_server) {
      _http->stop();
    }
  }

  // Detach WifiManager callbacks we wired in start(). The product will
  // rewire them after stop() returns.
  if (_wifi != nullptr) {
    _wifi->set_on_scan_complete(nullptr);
    _wifi->set_on_ap_client_joined(nullptr);
    _wifi->set_on_ap_client_left(nullptr);
    _wifi->set_on_got_ip(nullptr);
    _wifi->set_on_disconnected(nullptr);
    // Drop the AP and revert to STA-only. Preserves any successful STA
    // association we may have just earned (Connected → Stopped path).
    _wifi->set_mode(WifiMode::Sta);
  }
  _wifi = nullptr;
  _ble = nullptr;
  _http = nullptr;
  _set_state_locked(ProvisioningState::Idle);
  _mutex.unlock();

  _emit(info);
}

ProvisioningState ProvisioningManager::state() const {
  _mutex.lock();
  ProvisioningState s = _state;
  _mutex.unlock();
  return s;
}

void ProvisioningManager::send_ble_status(uint8_t /*status_code*/) {
  // Checkpoint 1: no BLE transport. Wired in checkpoint 2.
}

// ---------------------------------------------------------------------------
// Private event handlers (wired to WifiManager in start())
// ---------------------------------------------------------------------------

void ProvisioningManager::_on_scan_results(const WifiScanEntry *entries, uint16_t count) {
  _mutex.lock();
  _portal->update_scan_results(entries, count);
  _mutex.unlock();
}

void ProvisioningManager::_on_sta_connected(uint32_t ip) {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Connected;

  _mutex.lock();
  if (_state != ProvisioningState::Connecting) {
    _mutex.unlock();
    return;
  }
  info.data = _pending_data;
  info.ip = ip;
  _portal->set_state(WifiPortalTransport::PortalState::Connected);
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
    _mutex.unlock();
    return;
  }
  _portal->set_state(WifiPortalTransport::PortalState::Failed);
  _set_state_locked(ProvisioningState::WaitingForCredentials);
  _maybe_arm_timeout_locked();
  _mutex.unlock();
  _emit(info);
}

void ProvisioningManager::_on_ap_client_joined() {
  _mutex.lock();
  ++_ap_client_count;
  if (_ap_client_count == 1) {
    _pause_timeout_locked();
  }
  _mutex.unlock();
}

void ProvisioningManager::_on_ap_client_left() {
  _mutex.lock();
  if (_ap_client_count > 0) {
    --_ap_client_count;
  }
  if (_ap_client_count == 0) {
    _resume_timeout_locked();
  }
  _mutex.unlock();
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

void ProvisioningManager::_set_state_locked(ProvisioningState s) { _state = s; }

void ProvisioningManager::_emit(const ProvisioningEventInfo &info) {
  // Capture the callback under the lock so a concurrent stop() can't
  // race with a destroyed std::function. We then call the copy outside
  // the lock so reentrant calls into the manager would deadlock-free
  // (though the spec forbids reentry from callbacks).
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
    AG_LOGW(TAG, "credentials rejected — state is not WaitingForCredentials");
    _mutex.unlock();
    return false;
  }
  _pending_data = data;
  info.data = data;
  _portal->set_state(WifiPortalTransport::PortalState::Connecting);
  _set_state_locked(ProvisioningState::Connecting);
  _pause_timeout_locked();

  if (_wifi != nullptr) {
    if (data.has_static_ip()) {
      _wifi->set_static_ip(data.static_ip);
    } else {
      _wifi->clear_static_ip();
    }
    WifiStaConfig sta = {};
    std::strncpy(sta.ssid, data.ssid, sizeof(sta.ssid) - 1);
    std::strncpy(sta.password, data.password, sizeof(sta.password) - 1);
    // Single attempt — the provisioning manager surfaces ConnectFailed on
    // disconnect and the user can retry from the portal. WifiManager's
    // exponential-backoff retry is meant for runtime reconnection of a
    // known-good network, not for evaluating freshly-submitted creds.
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
    _mutex.unlock();
    return false;
  }
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
  if (_ap_client_count > 0) {
    // Spurious — clients reattached after the timer fired. Re-arm.
    _maybe_arm_timeout_locked();
    _mutex.unlock();
    return;
  }
  _timeout_armed = false;
  _dns->stop();

  // Same teardown sequence as stop() — always stops the HTTP server on
  // timeout since there is no product call to express a preference.
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
  _ble = nullptr;
  _http = nullptr;
  _set_state_locked(ProvisioningState::Idle);
  _mutex.unlock();
  _emit(info);
}

void ProvisioningManager::_maybe_arm_timeout_locked() {
  if (_config.overall_timeout_ms == 0) {
    return;
  }
  if (_state != ProvisioningState::WaitingForCredentials) {
    return;
  }
  if (_ap_client_count > 0) {
    // Paused while clients are connected.
    return;
  }
  _timer->arm(_config.overall_timeout_ms);
  _timeout_armed = true;
}

void ProvisioningManager::_pause_timeout_locked() {
  if (_timeout_armed) {
    _timer->cancel();
    _timeout_armed = false;
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
  }
}
