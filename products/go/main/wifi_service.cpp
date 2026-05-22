/**
 * AirGradient Go — WifiService implementation
 *
 * Stationary Wi-Fi mechanics. Mode policy lives in the orchestrator.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wifi_service.h"

#include "ag_log.h"
#include "go_events.h"
#include "services/wifi_manager.h"

#include <cstring>

static constexpr const char *TAG = "WifiService";

// Saved-credentials policy. Bounded retry; backoffs use WifiManager defaults.
static constexpr uint8_t STATIONARY_MAX_RETRY_COUNT = 5;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WifiService::WifiService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _wifi(deps.wifi), _ble(deps.ble), _http(deps.http), _cfg(cfg) {
  _install_wifi_callbacks();
}

WifiService::~WifiService() { _detach_wifi_callbacks(); }

// ---------------------------------------------------------------------------
// Credential queries
// ---------------------------------------------------------------------------

bool WifiService::has_saved_credentials() const { return _wifi.has_saved_credentials(); }

// ---------------------------------------------------------------------------
// Connection attempts
// ---------------------------------------------------------------------------

void WifiService::connect_with_saved_credentials(const WifiStaticIpConfig *static_ip) {
  _reset_deadline();
  _reset_online_latches();

  if (static_ip != nullptr && static_ip->ip != 0) {
    _wifi.set_static_ip(*static_ip);
  } else {
    _wifi.clear_static_ip();
  }

  _wifi.set_mode(WifiMode::Sta);

  WifiStaConfig sta{}; // ssid empty = NVS-saved credentials path
  sta.max_retry_count = STATIONARY_MAX_RETRY_COUNT;

  const WifiStatus status = _wifi.connect(sta);
  if (status == WifiStatus::NotFound) {
    // No persisted creds. Disconnect-policy router takes it from here.
    AG_LOGW(TAG, "saved-creds connect: no NVS credentials");
    _post_wifi_disconnected(WifiDisconnectReason::no_ap_found);
    return;
  }
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "saved-creds connect failed: %d", static_cast<int>(status));
    _post_wifi_disconnected(WifiDisconnectReason::unknown);
    return;
  }

  _arm_deadline(_cfg.initial_connect_window_ms);
}

void WifiService::try_default_fallback_credentials() {
  _reset_deadline();
  _reset_online_latches();
  _wifi.clear_static_ip();
  _wifi.set_mode(WifiMode::Sta);

  WifiStaConfig sta{};
  std::strncpy(sta.ssid, _cfg.fallback_ssid, sizeof(sta.ssid) - 1);
  std::strncpy(sta.password, _cfg.fallback_password, sizeof(sta.password) - 1);
  sta.max_retry_count = 0; // single-shot; no retry on fallback
  sta.persist = false;     // never write fallback creds to NVS

  const WifiStatus status = _wifi.connect(sta);
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "fallback connect failed: %d", static_cast<int>(status));
    _post_wifi_disconnected(WifiDisconnectReason::unknown);
    return;
  }

  _arm_deadline(_cfg.fallback_connect_window_ms);
}

// ---------------------------------------------------------------------------
// Provisioning (lands in CP2.3)
// ---------------------------------------------------------------------------

void WifiService::start_provisioning(ProvisioningTransport transport) {
  AG_LOGI(TAG, "start_provisioning(%u) — wired in CP2.3", static_cast<unsigned>(transport));
  _transport = transport;
  _provisioning_active = true;
}

void WifiService::switch_provisioning_transport() {
  AG_LOGI(TAG, "switch_provisioning_transport — wired in CP2.3");
}

void WifiService::stop_provisioning() {
  AG_LOGI(TAG, "stop_provisioning — wired in CP2.3");
  _provisioning_active = false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WifiService::shutdown() {
  _reset_deadline();
  _wifi.disconnect();
  _wifi.set_mode(WifiMode::Off);
  _detach_wifi_callbacks();
  _reset_online_latches();
  _provisioning_active = false;
}

void WifiService::clear_credentials() {
  _wifi.clear_saved_credentials();
  _reset_online_latches();
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool WifiService::is_online() const { return _online.load(); }

bool WifiService::is_connecting() const {
  return _initial_connect_deadline_ms != 0 && !_online.load();
}

bool WifiService::is_provisioning() const { return _provisioning_active; }

ProvisioningTransport WifiService::current_transport() const { return _transport; }

uint32_t WifiService::ip() const { return _ip.load(); }

int WifiService::rssi() const { return _rssi.load(); }

WifiDisconnectReason WifiService::last_disconnect_reason() const {
  return static_cast<WifiDisconnectReason>(_last_disconnect_reason.load());
}

bool WifiService::has_been_online() const { return _has_been_online.load(); }

// ---------------------------------------------------------------------------
// Timer integration
// ---------------------------------------------------------------------------

uint32_t WifiService::next_deadline_ms() const { return _initial_connect_deadline_ms; }

void WifiService::tick(uint32_t now_ms) {
  // Latched clear from on_got_ip (kept off the deadline writer thread).
  if (_clear_deadline_pending.exchange(false)) {
    _initial_connect_deadline_ms = 0;
  }

  if (_initial_connect_deadline_ms != 0 && now_ms >= _initial_connect_deadline_ms) {
    _initial_connect_deadline_ms = 0;
    AG_LOGW(TAG, "initial connect window expired");
    _post_wifi_disconnected(WifiDisconnectReason::connection_lost);
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void WifiService::_install_wifi_callbacks() {
  _wifi.set_on_got_ip([this](uint32_t ip) { _on_got_ip(ip); });
  _wifi.set_on_disconnected([this](WifiDisconnectReason r) { _on_disconnected(r); });
}

void WifiService::_detach_wifi_callbacks() {
  _wifi.set_on_got_ip(nullptr);
  _wifi.set_on_disconnected(nullptr);
}

void WifiService::_on_got_ip(uint32_t ip) {
  AG_LOGI(TAG, "wifi online: ip=0x%08x", static_cast<unsigned>(ip));
  _ip.store(ip);
  _online.store(true);
  _has_been_online.store(true);
  // Defer deadline clear to tick() so deadline stays single-writer.
  _clear_deadline_pending.store(true);
  _rssi.store(_wifi.status_snapshot().rssi);

  Event evt{};
  evt.type = EventType::WifiConnected;
  evt.wifi_ip = ip;
  RTOS::queue_send(_event_queue, &evt);
}

void WifiService::_on_disconnected(WifiDisconnectReason reason) {
  AG_LOGI(TAG, "wifi disconnected: reason=%u", static_cast<unsigned>(reason));
  _online.store(false);
  _last_disconnect_reason.store(static_cast<uint8_t>(reason));
  if (reason == WifiDisconnectReason::requested_by_user) {
    return; // synthetic teardown reason; not an external failure
  }
  _post_wifi_disconnected(reason);
}

void WifiService::_reset_deadline() {
  _initial_connect_deadline_ms = 0;
  _clear_deadline_pending.store(false);
}

void WifiService::_arm_deadline(uint32_t window_ms) {
  _initial_connect_deadline_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + window_ms;
}

void WifiService::_reset_online_latches() {
  _online.store(false);
  _has_been_online.store(false);
  _ip.store(0);
  _rssi.store(WIFI_RSSI_INVALID);
}

void WifiService::_post_wifi_disconnected(WifiDisconnectReason reason) {
  Event evt{};
  evt.type = EventType::WifiDisconnected;
  evt.wifi_disconnect_reason = static_cast<uint8_t>(reason);
  RTOS::queue_send(_event_queue, &evt);
}
