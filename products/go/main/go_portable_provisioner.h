/**
 * AirGradient Go — PortableWifiProvisioner
 *
 * Runs the existing Wi-Fi provisioning flow (scan, credentials, live status)
 * over the bonded Portable BLE link with no mode switch. Owns a Portable
 * ProvisioningManager on the attached transport: the prov + DIS services are
 * registered on the AgBleServer BleService advertises, and the radio is
 * powered on demand then dropped (verify-then-drop).
 *
 * Mechanics only — the orchestrator owns policy (mode gating, persistence,
 * teardown ordering) and routes events here. Threading: BLE writes are
 * marshaled (NimBLE task) to a PortableProvRequest the orchestrator drains;
 * blocking Wi-Fi work runs in handle_pending_request().
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_PORTABLE_PROVISIONER_H
#define GO_PORTABLE_PROVISIONER_H

#include <atomic>
#include <cstdint>

#include "rtos.h"
#include "types/provisioning_types.h"

class AgBleServer;
class GoBoard;
class ProvisioningManager;
class WifiManager;
struct ProvisioningEventInfo;

class PortableWifiProvisioner {
public:
  struct Deps {
    WifiManager &wifi; // borrowed; shared with BleService/Stationary, mode-exclusive
    AgBleServer &ble;  // borrowed; the same server BleService init's/advertises
    GoBoard &board;    // borrowed; for lazy init_wifi_subsystem()
  };

  struct Config {
    const char *ble_device_name = "AirGradient";
    const char *ble_model_name = nullptr;
    const char *ble_serial_number = nullptr;
    const char *ble_firmware_version = nullptr;
    // Drop the radio after this idle window (CONFIG_GO_PORTABLE_PROV_RADIO_IDLE_MS).
    uint32_t radio_idle_ms = 90000;
  };

  PortableWifiProvisioner(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg);
  ~PortableWifiProvisioner();

  PortableWifiProvisioner(const PortableWifiProvisioner &) = delete;
  PortableWifiProvisioner &operator=(const PortableWifiProvisioner &) = delete;

  // --- Lifecycle (orchestrator-driven; Portable and Stationary never overlap) ---

  /// Register prov + DIS on the borrowed server and park WaitingForCredentials
  /// with the radio off. Returns false on failure. Idempotent.
  bool attach();

  /// Abort in-flight scan/connect, drop the radio, and detach from the server
  /// without deinitialising it (BleService deinits later). Idempotent.
  void stop();

  // --- Orchestrator-task request driver ---

  /// Drain the pending BLE request: bring the radio up, arm the idle timer,
  /// drive the manager scan/connect (reject on Wi-Fi bring-up failure).
  void handle_pending_request();

  // --- Result routing (orchestrator forwards) ---

  /// Verify-then-drop: drop the radio and reopen the session for re-provision.
  void on_connected();

  /// Drop the radio on BLE disconnect (no-op if already off).
  void on_ble_disconnected();

  // --- Queries ---

  /// True while the radio is powered. Gates History export (contention).
  bool is_radio_active() const;

  /// True between attach() and stop().
  bool is_attached() const;

  // --- Timer integration (driven by the orchestrator's event loop) ---

  /// Absolute ms of the radio-idle deadline, or 0 when the radio is off.
  uint32_t next_deadline_ms() const;

  /// Drop the radio once the idle deadline passes. Idempotent.
  void tick(uint32_t now_ms);

#ifdef TEST_HOST
  friend class PortableProvTestAccess;
#endif

private:
  // NimBLE task: buffer the request + post PortableProvRequest.
  void _on_attached_request(const AttachedRequest &req);
  // Wi-Fi/NimBLE task: marshal the manager result event onto the queue.
  void _on_provisioning_event(const ProvisioningEventInfo &info);

  // Orchestrator-task helpers.
  bool _ensure_wifi_ready();
  void _drop_radio();
  void _arm_idle_timer();
  void _cancel_idle_timer();

  RtosQueueHandle _event_queue;
  WifiManager &_wifi;
  AgBleServer &_ble;
  GoBoard &_board;
  Config _cfg;

  // Forward-declarable; .cpp owns new/delete (mirrors WifiService::_prov).
  ProvisioningManager *_manager = nullptr;

  bool _attached = false;
  bool _wifi_initialized = false; // init_wifi_subsystem() run at least once

  // Written on the orchestrator task, read lock-free by History gating.
  std::atomic<bool> _radio_active{false};

  // Radio-idle deadline; orchestrator-task single-writer.
  uint32_t _radio_idle_deadline_ms = 0;

  // Pending request (NimBLE -> orchestrator task); single slot, last-write-wins.
  RtosMutex _pending_mutex;
  AttachedRequest _pending_request{};
  bool _pending_valid = false;
};

#endif // GO_PORTABLE_PROVISIONER_H
