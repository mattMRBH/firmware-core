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
#include <functional>

#include "hal/ota_image_writer.h"
#include "rtos.h"
#include "types/ota_types.h"

class AgBleServer;
class AgBleCharacteristic;

// BLE push OTA service. Owns the OTA GATT service (Control / Data / Status) on
// a borrowed, already-init'd + secured AgBleServer that is NOT yet advertising,
// and drives the universal OtaImageWriter from the phone-pushed image bytes.
//
// The phone is the orchestrator: it writes START / image chunks / END on the
// Control and Data characteristics; the device flashes each chunk on a worker
// task spawned on START and reports state over the Status NOTIFY. There is no
// OtaUpdater and no OtaImageSource on this path. Reboot is never performed
// here — the product decides from the terminal on_event(Done/Failed, status).
//
// Threading: every OtaImageWriter call (begin/write/finish/abort) runs on the
// worker task, never on the NimBLE host task. A single chunk buffer is kept
// race-free by the consumed_sem handshake: the Data write callback blocks until
// the worker has flashed the chunk, which is also the backpressure that paces
// the phone to flash speed.
//
// Borrowed-server contract (attached only): setup() registers the GATT service
// + characteristics and MUST be called before the product calls
// start_advertising(). teardown()/the destructor clear the callbacks and abort
// any in-flight transfer but never init()/deinit() the server.
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

  // Register the OTA GATT service + Control/Data/Status characteristics and
  // their write callbacks on the borrowed server. The server must already be
  // init()'d/secured but NOT yet advertising — call this before
  // start_advertising(). Returns false on registration failure.
  bool setup();

  // Clear characteristic write callbacks, abort any in-flight transfer, free
  // the chunk buffer, and delete the worker (bounded wait on _worker_exited).
  // Never deinit()s the server. Idempotent. MUST NOT be called from on_event
  // or the worker task (it would self-join) — call from the product/owner
  // context only.
  void teardown();

  // Product forwards the borrowed server's disconnect here so an in-flight
  // transfer is aborted when the central drops. Safe when idle.
  void handle_disconnect();

  // Fired on each lifecycle transition: Downloading (start/ready), Applying,
  // Done (Ok), Failed (status). `result` is meaningful on the terminal states
  // (Done/Failed) and is Ok during progress. Set before setup().
  //
  // Context: all lifecycle events fire on the worker task. The only exception
  // is a pre-spawn rejection (malformed/oversized START or task_create
  // failure), which fires Failed on the NimBLE host task. Callbacks must not
  // block or re-enter OtaBleService (in particular, must not call teardown()).
  void set_on_event(std::function<void(OtaState, OtaStatus)> cb);

  // True between the start edge (START accepted + worker spawned) and the
  // terminal edge of a transfer. Backed by a std::atomic<bool>, so it is safe
  // to read from any task (power manager, UI, BLE-coexistence gating). The only
  // cross-task field OtaBleService exposes; cleared before the terminal
  // on_event fires, so a callback that re-checks is_active() already sees false.
  bool is_active() const;

private:
  // Internal transfer state machine.
  enum class State : uint8_t { Idle, Starting, Downloading, Applying };

  // Worker command channel values (one notification, three intents).
  enum WorkerCmd : uint32_t { CMD_CHUNK = 1, CMD_FINISH = 2, CMD_ABORT = 3 };

  // NimBLE-host-task write handlers.
  void _on_control_write(const uint8_t *data, size_t len);
  void _on_data_write(const uint8_t *data, size_t len);

  // Control op dispatch (decoded on the NimBLE host task).
  void _handle_start(const void *map);
  void _handle_end();
  void _handle_abort();

  // Pre-spawn rejection: emit Failed{status} on the NimBLE host task without a
  // worker (malformed/oversized START, alloc / task_create failure). Stays Idle.
  void _reject_prespawn(OtaStatus status);

  // Signal the worker to run its abort terminal with the given reason. Sets the
  // carried reason then notifies CMD_ABORT (no-op under TEST_HOST).
  void _request_abort(OtaStatus status, bool send_notify);

  // Worker task body (hardware only) and its directly-invokable steps.
  void _worker_loop();
  bool _begin_step();  // writer.begin(); emit ready. false on begin failure.
  bool _drain_one();   // writer.write() one chunk; false on write error.
  void _finish_step(); // truncation guard -> Applying -> finish() -> terminal.
  void _terminate(OtaStatus status, bool send_notify); // abort() -> Failed terminal.

  // Terminal/progress emission (NOTIFY {state,result} + on_event).
  void _emit_event(OtaState state, OtaStatus status);
  void _emit_terminal(OtaState state, OtaStatus status, bool send_notify);
  void _notify_status(OtaState state, OtaStatus status);

  // Worker self-cleanup after the loop breaks (hardware only): signal the
  // teardown waiter and self-delete. Touches no member after _worker_exited.
  void _worker_finalize();

  // Spawn the worker on a valid START (real task on hardware; no-op spawn on
  // host so tests can pump the steps). Returns true if the transfer may proceed.
  bool _spawn_worker();

  // Trampoline into _worker_loop() (RTOS::task_create entry).
  static void _worker_entry(void *arg);

  AgBleServer &_server;
  OtaImageWriter &_writer;

  AgBleCharacteristic *_control_char = nullptr;
  AgBleCharacteristic *_data_char = nullptr;
  AgBleCharacteristic *_status_char = nullptr;

  std::function<void(OtaState, OtaStatus)> _on_event;

  // Transfer state. _state is conceptually single-owner (paced by the phone
  // waiting for the ready NOTIFY); is_active is the only cross-task field.
  State _state = State::Idle;
  std::atomic<bool> _is_active{false};

  size_t _total = 0;          // declared image size from START
  size_t _bytes_accepted = 0; // bytes copied + queued by the Data handler

  // Throttles the progress INFO log (reuses CONFIG_AG_OTA_PROGRESS_INTERVAL_MS).
  uint64_t _last_progress_log_ms = 0;

  // Single chunk buffer, heap-allocated on START and freed at terminal.
  uint8_t *_chunk_buf = nullptr;
  size_t _chunk_len = 0;

  // One-slot handshake: the Data callback blocks on _consumed_sem until the
  // worker has flashed the chunk (backpressure + single-buffer safety).
  RtosBinarySemaphore _consumed_sem;
  // Signalled by the worker as its last act so teardown can join (bounded).
  RtosBinarySemaphore _worker_exited;
  RtosTaskHandle _worker = nullptr;

  // Abort reason carried from the signalling context to the worker terminal.
  OtaStatus _terminal_status = OtaStatus::Aborted;
  bool _suppress_notify = false; // true on disconnect (link already gone)

#ifdef TEST_HOST
  // Host tests drive the worker steps directly (no real FreeRTOS task) and
  // assert internal state via this friend; see ota_ble_service_test_access.h.
  friend class OtaBleServiceTestAccess;
#endif
};

#endif // AG_OTA_BLE_SERVICE_H
