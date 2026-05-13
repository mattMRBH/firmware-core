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
class CaptiveDnsResponder;
class ProvisioningTimer;

/// Wi-Fi provisioning manager.
///
/// Owns the provisioning state machine and coordinates the Wi-Fi
/// captive-portal transport (and, in checkpoint 2, the BLE transport)
/// to collect Wi-Fi credentials from the user.
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
  /// routes, starts the HTTP server on `config.http_port`, and starts
  /// the captive DNS responder. The BLE transport is wired in
  /// checkpoint 2 — until then the `ble` parameter is accepted but
  /// ignored.
  ///
  /// Contract: the caller-supplied HttpServer must NOT be started yet
  /// and must NOT have any routes registered. ProvisioningManager owns
  /// the HTTP server's lifecycle and route table for the duration of
  /// the provisioning session.
  ///
  /// @param wifi   Wi-Fi manager — borrowed for AP, scan, STA connect
  /// @param ble    BLE server — nullable during checkpoint 1
  /// @param http   HTTP server — fresh instance, not yet started
  /// @param config provisioning configuration
  /// @return false if already running or arguments are invalid
  bool start(WifiManager &wifi, AgBleServer *ble, HttpServer &http,
             const ProvisioningConfig &config);

  /// Stop provisioning. Wipes all HTTP routes, reverts Wi-Fi to STA
  /// mode (drops the AP), tears down the DNS responder. Fires the
  /// Stopped event.
  ///
  /// @param stop_http_server when true (default), also stops the HTTP
  ///        server. Pass false to keep the server running so the
  ///        product can register its own routes immediately without a
  ///        bind/unbind cycle.
  void stop(bool stop_http_server = true);

  /// Current provisioning state.
  ProvisioningState state() const;

  /// Send an application-level status code over BLE. Valid between
  /// the Connected event and stop(). No-op when the BLE transport is
  /// not active (which is always the case in checkpoint 1).
  void send_ble_status(uint8_t status_code);

  // -- Visible for tests --------------------------------------------------
  //
  // Host tests construct a manager with no live transports, drive it
  // via direct API calls, and observe events through the registered
  // callback. The portal HTML symbols are provided by the firmware
  // build; in TEST_HOST builds they default to nullptr so the portal
  // transport registers only the API routes.

  /// Inject scan results from outside (e.g. wired from
  /// WifiManager::on_scan_complete in production code or a fake in
  /// tests). Forwarded to the portal transport's cache.
  void inject_scan_results(const WifiScanEntry *entries, uint16_t count);

  /// Notify the manager that the Wi-Fi STA either connected or failed.
  /// Used by the production wiring (from WifiManager callbacks) and by
  /// host tests to drive the state machine.
  void notify_sta_connected(uint32_t ip);
  void notify_sta_disconnected();

  /// AP client connect/disconnect (drives the inactivity-timeout
  /// pause/resume logic). Used by both production wiring and tests.
  void notify_ap_client_joined();
  void notify_ap_client_left();

#ifdef TEST_HOST
  /// Synchronously fire the inactivity timer as if it had expired.
  void fire_timeout_for_test();

  /// Access the underlying portal transport (for handler-level tests).
  WifiPortalTransport &portal_for_test() { return *_portal; }
#endif

private:
  void _emit(const ProvisioningEventInfo &info);
  void _set_state_locked(ProvisioningState s);
  bool _accept_credentials(const ProvisioningData &data);
  bool _trigger_scan();
  void _on_timeout();
  void _maybe_arm_timeout_locked();
  void _pause_timeout_locked();
  void _resume_timeout_locked();

  mutable RtosMutex _mutex;
  ProvisioningState _state = ProvisioningState::Idle;
  ProvisioningEventCallback _on_event;

  // Borrowed dependencies — set in start(), cleared in stop().
  WifiManager *_wifi = nullptr;
  AgBleServer *_ble = nullptr;
  HttpServer *_http = nullptr;

  ProvisioningConfig _config = {};
  ProvisioningData _pending_data = {};

  uint32_t _ap_client_count = 0;
  bool _timeout_armed = false;
  uint64_t _timeout_deadline_ms = 0;

  std::unique_ptr<WifiPortalTransport> _portal;
  std::unique_ptr<CaptiveDnsResponder> _dns;
  std::unique_ptr<ProvisioningTimer> _timer;
};

#endif // AG_PROVISIONING_MANAGER_H
