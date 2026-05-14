/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_MANAGER_H
#define AG_PROVISIONING_MANAGER_H

#include <cstdint>
#include <memory>

#include "../types/provisioning_types.h"
#include "rtos.h"

class WifiManager;
class AgBleServer;
class HttpServer;
class WifiPortalTransport;
class BleTransport;
class CaptiveDnsResponder;
class ProvisioningTimer;

/// Wi-Fi provisioning manager.
///
/// Owns the provisioning state machine and coordinates two transports
/// (Wi-Fi captive portal and BLE GATT) to collect Wi-Fi credentials
/// from the user.
///
/// Product code creates WifiManager, AgBleServer, and HttpServer, then
/// passes them to start(). The provisioning manager borrows these
/// objects for the duration of the provisioning session. Product code
/// resumes full ownership after stop() returns.
///
/// Lifecycle events are delivered via a single callback set with
/// set_on_event(). Callbacks fire from various task contexts (WiFi
/// event loop, HTTP server, NimBLE). Callers must not block inside
/// callbacks or call back into ProvisioningManager.
///
/// ISR-safe: no
/// Thread-safe: yes (internal mutex)
/// Blocking: no (all outcomes delivered via callbacks)
class ProvisioningManager {
public:
  ProvisioningManager();
  ~ProvisioningManager();

  ProvisioningManager(const ProvisioningManager &) = delete;
  ProvisioningManager &operator=(const ProvisioningManager &) = delete;

  /// Register the event callback. Must be called before start().
  void set_on_event(ProvisioningEventCallback cb);

  /// Start provisioning. Non-blocking.
  ///
  /// Switches Wi-Fi to ApSta mode, starts the AP, registers HTTP
  /// routes, starts the HTTP server on `config.http_port`, starts the
  /// captive DNS responder, initialises the BLE server, creates GATT
  /// services, and begins BLE advertising.
  ///
  /// Contract: the caller-supplied HttpServer must NOT be started yet
  /// and must NOT have any routes registered. The caller-supplied
  /// AgBleServer must NOT be initialised yet. ProvisioningManager owns
  /// both the HTTP server's and BLE server's lifecycle for the
  /// duration of the provisioning session.
  ///
  /// @param wifi   Wi-Fi manager — borrowed for AP, scan, STA connect
  /// @param ble    BLE server — borrowed for GATT provisioning service
  /// @param http   HTTP server — fresh instance, not yet started
  /// @param config provisioning configuration
  /// @return false if already running or arguments are invalid
  bool start(WifiManager &wifi, AgBleServer &ble, HttpServer &http,
             const ProvisioningConfig &config);

  /// Stop provisioning. Wipes all HTTP routes, reverts Wi-Fi to STA
  /// mode (drops the AP), tears down the DNS responder, deinits the
  /// BLE server. Fires the Stopped event.
  ///
  /// @param stop_http_server when true (default), also stops the HTTP
  ///        server. Pass false to keep the server running so the
  ///        product can register its own routes immediately without a
  ///        bind/unbind cycle.
  void stop(bool stop_http_server = true);

  /// Current provisioning state.
  ProvisioningState state() const;

  /// Send an application-level status code over BLE. Valid between
  /// the Connected event and stop(). No-op when provisioning is not
  /// running.
  void send_ble_status(uint8_t status_code);

private:
#ifdef TEST_HOST
  friend class ProvisioningTestAccess;
#endif

  // -- Event handlers (wired to WifiManager / BLE callbacks in start()) --
  void _on_sta_connected(uint32_t ip);
  void _on_sta_disconnected();
  void _on_ap_client_joined();
  void _on_ap_client_left();
  void _on_ble_client_connected();
  void _on_ble_client_disconnected();
  void _on_scan_results(const WifiScanEntry *entries, uint16_t count);

  void _emit(const ProvisioningEventInfo &info);
  void _set_state_locked(ProvisioningState s);
  bool _accept_credentials(const ProvisioningData &data);
  bool _trigger_scan();
  void _on_timeout();
  void _maybe_arm_timeout_locked();
  void _pause_timeout_locked();
  void _resume_timeout_locked();
  uint32_t _total_client_count_locked() const;

  mutable RtosMutex _mutex;
  ProvisioningState _state = ProvisioningState::Idle;
  ProvisioningEventCallback _on_event;

  // Borrowed dependencies — set in start(), cleared in stop().
  WifiManager *_wifi = nullptr;
  HttpServer *_http = nullptr;

  ProvisioningConfig _config = {};
  ProvisioningData _pending_data = {};

  uint32_t _ap_client_count = 0;
  uint32_t _ble_client_count = 0;
  bool _timeout_armed = false;

  std::unique_ptr<WifiPortalTransport> _portal;
  std::unique_ptr<BleTransport> _ble_transport;
  std::unique_ptr<CaptiveDnsResponder> _dns;
  std::unique_ptr<ProvisioningTimer> _timer;
};

#endif // AG_PROVISIONING_MANAGER_H
