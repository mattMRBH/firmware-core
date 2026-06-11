/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_BLE_SERVICE_H
#define AG_OTA_BLE_SERVICE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hal/ota_image_writer.h"
#include "rtos.h"
#include "types/ota_types.h"

class AgBleServer;
class AgBleCharacteristic;

// BLE push OTA service (v2). Owns the OTA GATT service (Control/Data/Status)
// on a borrowed, init'd+secured, not-yet-advertising AgBleServer and drives the
// universal OtaImageWriter from phone-pushed bytes. The phone orchestrates; no
// OtaUpdater/OtaImageSource. Reboot is the product's call.
//
// Two contexts, no worker task / no chunk buffer:
//   - Host task: Control/Data write callbacks. Data is flashed directly in the
//     callback (writer.write()); Control latches a command + wakes run().
//   - Product task: poll() then run(). The stack-hungry begin()/finish()/abort()
//     and all Status NOTIFYs (bar a pre-validation START reject) run here.
// The two never touch the writer concurrently: the phone waits for the
// Downloading NOTIFY before sending Data, END follows the last Data callback,
// and any terminal latch flips _pending (atomic) so later Data writes no-op.
//
// Attached-only: setup() registers GATT and MUST precede start_advertising();
// teardown()/dtor clear callbacks + abort in-flight but never init/deinit.
class OtaBleService {
public:
  // Borrows an init'd + secured AgBleServer that is NOT yet advertising, and
  // the universal writer. Neither is owned; the writer must outlive the
  // service. setup() must be called before the product calls
  // start_advertising().
  OtaBleService(AgBleServer &server, OtaImageWriter &writer);
  ~OtaBleService();

  OtaBleService(const OtaBleService &) = delete;
  OtaBleService &operator=(const OtaBleService &) = delete;

  // Register the OTA GATT service + characteristics on the borrowed server.
  // Must run before start_advertising(). Returns false on registration failure.
  bool setup();

  // Idle-phase poll on the product task. Non-blocking by default; pass a timeout
  // to park up to timeout_ms waiting for a START. Returns Starting once a valid
  // START is latched (is_active() is already true), else Idle. If it returns
  // Starting the product MUST call run().
  OtaState poll(uint32_t timeout_ms = 0);

  // Drive one transfer to its terminal on the product task, AFTER poll()
  // returned Starting. Runs begin() (+ready NOTIFY), blocks servicing
  // END/ABORT/disconnect/stall while Data callbacks flash, then finish()/abort()
  // + terminal NOTIFY. Returns the final OtaStatus (Ok on success).
  OtaStatus run();

  // Clear write callbacks and abort any in-flight transfer; never deinit()s the
  // server. Idempotent. Product/owner context only.
  void teardown();

  // Forward the server's disconnect so an in-flight transfer is aborted. Safe
  // when idle. Runs on the NimBLE host callback context.
  void handle_disconnect();

  // True between the start edge (START accepted, before begin()) and the
  // terminal edge. Atomic; safe to read from any task for sleep/coexistence
  // gating.
  bool is_active() const;

private:
  // Internal transfer state machine.
  enum class State : uint8_t { Idle, Starting, Downloading, Applying };

  // Pending command latched by the host task for run() to act on.
  enum class Cmd : uint8_t { None, Finish, Abort };

  // NimBLE-host-task write handlers.
  void _on_control_write(const uint8_t *data, size_t len);
  void _on_data_write(const uint8_t *data, size_t len);

  // Control op dispatch (decoded on the NimBLE host task).
  void _handle_start(const void *map);
  void _handle_end();
  void _handle_abort();

  // Pre-validation rejection: emit Failed{status} on the NimBLE host task
  // before any transfer is live (malformed/oversized START). Stays Idle.
  void _reject_start(OtaStatus status);

  // Latch a pending abort with its reason then wake run(); does NOT touch the
  // writer (run() runs writer.abort()). Called from the host task / disconnect.
  void _latch_abort(OtaStatus status);

  // Directly-invokable steps run by run() on the product task (host-testable).
  bool _begin_step();                // writer.begin(); emit ready. false on begin failure.
  void _finish_step();               // truncation guard -> Applying -> finish() -> terminal.
  void _terminate(OtaStatus status); // abort() -> Failed terminal.

  // Status emission. The terminal NOTIFY is always attempted; on a dropped link
  // it simply no-ops (no subscriber), so there is no separate suppress path.
  void _emit_terminal(OtaState state, OtaStatus status);
  void _notify_status(OtaState state, OtaStatus status);

  AgBleServer &_server;
  OtaImageWriter &_writer;

  AgBleCharacteristic *_control_char = nullptr;
  AgBleCharacteristic *_data_char = nullptr;
  AgBleCharacteristic *_status_char = nullptr;

  // _state: product task owns the Starting->Downloading->Applying transitions;
  // the host task only reads it (and sets Starting at START). Cross-task
  // ordering comes from the _signal give/take and the host lock around NOTIFY.
  State _state = State::Idle;
  std::atomic<bool> _is_active{false};

  // Latched by the host task before waking run(); also the Data-callback no-op
  // guard once a terminal is pending.
  std::atomic<Cmd> _pending{Cmd::None};
  OtaStatus _terminal_status = OtaStatus::Aborted; // abort reason for run()

  size_t _total = 0;                      // declared image size from START
  std::atomic<size_t> _bytes_accepted{0}; // bytes flashed by the Data callback
  OtaStatus _result = OtaStatus::Ok;      // terminal result run() returns

  // Given by the host task on START/END/ABORT/disconnect/data error; poll()/
  // run() block on it (run() with the stall timeout).
  RtosBinarySemaphore _signal;

#ifdef TEST_HOST
  // Host tests drive the steps directly (no real run() loop) and assert
  // internal state via this friend; see ota_ble_service_test_access.h.
  friend class OtaBleServiceTestAccess;
#endif
};

#endif // AG_OTA_BLE_SERVICE_H
