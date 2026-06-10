/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/ota_ble_service.h"

#include <cstring>
#include <new>

#include <cbor.h>

#include "ag_log.h"
#include "hal/ble_server.h"
#include "services/ota_ble_protocol.h"

// Pull in Kconfig values when building with ESP-IDF; fall back to the spec
// defaults for native host-test builds where no sdkconfig.h exists.
#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_AG_OTA_BLE_CHUNK_SIZE
#define CONFIG_AG_OTA_BLE_CHUNK_SIZE 512
#endif
#ifndef CONFIG_AG_OTA_BLE_CONTROL_MAX_BYTES
#define CONFIG_AG_OTA_BLE_CONTROL_MAX_BYTES 64
#endif
#ifndef CONFIG_AG_OTA_BLE_FW_MAX_LEN
#define CONFIG_AG_OTA_BLE_FW_MAX_LEN 32
#endif
#ifndef CONFIG_AG_OTA_BLE_STALL_TIMEOUT_MS
#define CONFIG_AG_OTA_BLE_STALL_TIMEOUT_MS 10000
#endif
#ifndef CONFIG_AG_OTA_BLE_WRITE_TIMEOUT_MS
#define CONFIG_AG_OTA_BLE_WRITE_TIMEOUT_MS 5000
#endif
#ifndef CONFIG_AG_OTA_BLE_WORKER_STACK_SIZE
#define CONFIG_AG_OTA_BLE_WORKER_STACK_SIZE 8192
#endif
#ifndef CONFIG_AG_OTA_BLE_WORKER_PRIORITY
#define CONFIG_AG_OTA_BLE_WORKER_PRIORITY 5
#endif
// Reused to throttle the progress INFO log (same knob as the pull path).
#ifndef CONFIG_AG_OTA_PROGRESS_INTERVAL_MS
#define CONFIG_AG_OTA_PROGRESS_INTERVAL_MS 250
#endif

namespace {

constexpr const char *TAG = "OtaBle";

// GATT UUIDs — PLACEHOLDERS, pending allocation alongside the AirGradient
// provisioning (acbcfea8-...) and Go data-service UUIDs. Swap these for the
// assigned values once allocated (see spec "Open Questions").
constexpr const char *OTA_SERVICE_UUID = "ab9a0001-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_CONTROL_CHAR_UUID = "ab9a0002-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_DATA_CHAR_UUID = "ab9a0003-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_STATUS_CHAR_UUID = "ab9a0004-1e3c-4f5a-9b6d-0a1b2c3d4e5f";

// Control op string buffer — bounds the longest accepted op ("start") + NUL.
constexpr size_t OP_BUF_LEN = 8;

// Status NOTIFY CBOR buffer — {state:u8, result:u8} with headroom.
constexpr size_t STATUS_CBOR_BUF_SIZE = 32;

// Extra margin over WRITE_TIMEOUT for the bounded teardown join, covering the
// worker's terminal/abort work after it releases the handshake.
constexpr uint32_t TEARDOWN_MARGIN_MS = 1000;

} // namespace

OtaBleService::OtaBleService(AgBleServer &server, OtaImageWriter &writer)
    : _server(server), _writer(writer) {}

OtaBleService::~OtaBleService() { teardown(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool OtaBleService::setup() {
  AgBleGattService *svc = _server.add_service(OTA_SERVICE_UUID);
  if (svc == nullptr) {
    AG_LOGE(TAG, "add OTA service failed");
    return false;
  }

  constexpr uint16_t write_props = AgBleProperty::WRITE | AgBleProperty::WRITE_AUTHEN;
  _control_char = svc->add_characteristic(OTA_CONTROL_CHAR_UUID, write_props);
  if (_control_char == nullptr) {
    AG_LOGE(TAG, "add Control characteristic failed");
    return false;
  }

  _data_char = svc->add_characteristic(OTA_DATA_CHAR_UUID, write_props);
  if (_data_char == nullptr) {
    AG_LOGE(TAG, "add Data characteristic failed");
    return false;
  }

  // Status is NOTIFY-only with no readable value. READ_AUTHEN (without READ)
  // gates the CCCD subscription on an authenticated link.
  constexpr uint16_t status_props = AgBleProperty::NOTIFY | AgBleProperty::READ_AUTHEN;
  _status_char = svc->add_characteristic(OTA_STATUS_CHAR_UUID, status_props);
  if (_status_char == nullptr) {
    AG_LOGE(TAG, "add Status characteristic failed");
    return false;
  }

  _control_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { _on_control_write(data, len); });
  _data_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { _on_data_write(data, len); });

  if (!svc->start()) {
    AG_LOGE(TAG, "OTA service start failed");
    return false;
  }

  AG_LOGI(TAG, "OTA GATT registered (Control/Data/Status)");
  return true;
}

void OtaBleService::teardown() {
  // Clear write callbacks first so a late write cannot re-enter us.
  if (_control_char != nullptr) {
    _control_char->set_write_callback(nullptr);
  }
  if (_data_char != nullptr) {
    _data_char->set_write_callback(nullptr);
  }

  if (is_active()) {
    AG_LOGI(TAG, "teardown: aborting in-flight transfer");
    _request_abort(OtaStatus::Aborted, /*send_notify=*/true);

    // Bounded join — no force-delete. The worker self-deletes after releasing
    // the handshake; only a hardware flash wedge can expire this wait.
    if (_worker_exited.is_created() &&
        !_worker_exited.take(CONFIG_AG_OTA_BLE_WRITE_TIMEOUT_MS + TEARDOWN_MARGIN_MS)) {
      AG_LOGE(TAG, "worker wedged in flash write; reboot required (no force-delete)");
      // Do NOT free the buffer, force-delete the task, or return to Idle: the
      // task may still be inside esp_ota_write(). Recovery is a reboot.
      return;
    }
  }

  // The worker freed _chunk_buf in its terminal step before signalling
  // _worker_exited, so this is a null no-op on hardware. On the host path
  // (no worker ran) it releases the buffer allocated at START.
  delete[] _chunk_buf;
  _chunk_buf = nullptr;
  _chunk_len = 0;

  _control_char = nullptr;
  _data_char = nullptr;
  _status_char = nullptr;
  _worker = nullptr;
  _state = State::Idle;
}

void OtaBleService::handle_disconnect() {
  if (!is_active()) {
    return;
  }
  AG_LOGI(TAG, "disconnect: aborting in-flight transfer");
  // Link is already gone — no NOTIFY can reach the phone.
  _request_abort(OtaStatus::TransportError, /*send_notify=*/false);
}

void OtaBleService::set_on_event(std::function<void(OtaState, OtaStatus)> cb) {
  _on_event = std::move(cb);
}

bool OtaBleService::is_active() const { return _is_active.load(); }

// ---------------------------------------------------------------------------
// Control characteristic (NimBLE host task)
// ---------------------------------------------------------------------------

void OtaBleService::_on_control_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0) {
    return;
  }
  if (len > CONFIG_AG_OTA_BLE_CONTROL_MAX_BYTES) {
    AG_LOGW(TAG, "oversized control write: %u bytes", static_cast<unsigned>(len));
    if (_state == State::Idle) {
      _reject_prespawn(OtaStatus::InvalidArgument);
    }
    return;
  }

  CborParser parser;
  CborValue map;
  if (cbor_parser_init(data, len, 0, &parser, &map) != CborNoError || !cbor_value_is_map(&map)) {
    AG_LOGW(TAG, "malformed control CBOR");
    if (_state == State::Idle) {
      _reject_prespawn(OtaStatus::InvalidArgument);
    }
    return;
  }

  CborValue op_val;
  if (cbor_value_map_find_value(&map, OTA_BLE_KEY_OP, &op_val) != CborNoError ||
      !cbor_value_is_text_string(&op_val)) {
    AG_LOGW(TAG, "control missing 'op'");
    if (_state == State::Idle) {
      _reject_prespawn(OtaStatus::InvalidArgument);
    }
    return;
  }

  char op[OP_BUF_LEN];
  size_t op_len = sizeof(op);
  if (cbor_value_copy_text_string(&op_val, op, &op_len, nullptr) != CborNoError) {
    AG_LOGW(TAG, "control 'op' too long / unreadable");
    if (_state == State::Idle) {
      _reject_prespawn(OtaStatus::InvalidArgument);
    }
    return;
  }

  if (std::strcmp(op, OTA_BLE_OP_START) == 0) {
    _handle_start(&map);
  } else if (std::strcmp(op, OTA_BLE_OP_END) == 0) {
    _handle_end();
  } else if (std::strcmp(op, OTA_BLE_OP_ABORT) == 0) {
    _handle_abort();
  } else {
    AG_LOGW(TAG, "unknown control op '%s'", op);
  }
}

void OtaBleService::_handle_start(const void *map_ptr) {
  if (_state != State::Idle) {
    AG_LOGW(TAG, "START while active — rejected (no second worker)");
    return;
  }

  const CborValue *map = static_cast<const CborValue *>(map_ptr);

  // total (required, non-zero, fits u32).
  CborValue total_val;
  if (cbor_value_map_find_value(map, OTA_BLE_KEY_TOTAL, &total_val) != CborNoError ||
      !cbor_value_is_unsigned_integer(&total_val)) {
    AG_LOGW(TAG, "START missing/invalid 'total'");
    _reject_prespawn(OtaStatus::InvalidArgument);
    return;
  }
  uint64_t total = 0;
  cbor_value_get_uint64(&total_val, &total);
  if (total == 0 || total > UINT32_MAX) {
    AG_LOGW(TAG, "START rejected: total=%llu", static_cast<unsigned long long>(total));
    _reject_prespawn(OtaStatus::InvalidArgument);
    return;
  }

  // fw (informational, optional). Truncation against FW_MAX_LEN is a reject.
  CborValue fw_val;
  char fw[CONFIG_AG_OTA_BLE_FW_MAX_LEN + 1] = {0};
  if (cbor_value_map_find_value(map, OTA_BLE_KEY_FW, &fw_val) == CborNoError &&
      cbor_value_is_text_string(&fw_val)) {
    size_t fw_len = sizeof(fw);
    if (cbor_value_copy_text_string(&fw_val, fw, &fw_len, nullptr) != CborNoError) {
      AG_LOGW(TAG, "START 'fw' exceeds %d bytes", CONFIG_AG_OTA_BLE_FW_MAX_LEN);
      _reject_prespawn(OtaStatus::InvalidArgument);
      return;
    }
  }
  AG_LOGI(TAG, "START: total=%u fw='%s'", static_cast<unsigned>(total), fw);

  // Allocate the single chunk buffer (freed at the terminal step).
  _chunk_buf = new (std::nothrow) uint8_t[CONFIG_AG_OTA_BLE_CHUNK_SIZE];
  if (_chunk_buf == nullptr) {
    AG_LOGE(TAG, "chunk buffer allocation failed");
    _reject_prespawn(OtaStatus::TransportError);
    return;
  }

  // Reset the handshake/join semaphores so a leftover give from a previous
  // transfer cannot let this transfer's first take() skip the handshake.
  _consumed_sem.destroy();
  _consumed_sem.create();
  _worker_exited.destroy();
  _worker_exited.create();

  _total = static_cast<size_t>(total);
  _bytes_accepted = 0;
  _chunk_len = 0;
  _last_progress_log_ms = 0;
  _state = State::Starting;

  if (!_spawn_worker()) {
    AG_LOGE(TAG, "worker task_create failed");
    delete[] _chunk_buf;
    _chunk_buf = nullptr;
    _consumed_sem.destroy();
    _worker_exited.destroy();
    _state = State::Idle;
    _reject_prespawn(OtaStatus::TransportError);
    return;
  }
  // is_active is set true inside _spawn_worker (before begin()) so the product
  // can inhibit sleep during the partition erase.
}

void OtaBleService::_handle_end() {
  if (_state == State::Downloading) {
    RTOS::task_notify_send(_worker, CMD_FINISH);
  }
  // END while Idle/Starting/Applying: ignored.
}

void OtaBleService::_handle_abort() {
  if (_state == State::Starting || _state == State::Downloading || _state == State::Applying) {
    AG_LOGI(TAG, "ABORT received from phone");
    _request_abort(OtaStatus::Aborted, /*send_notify=*/true);
  }
  // ABORT while Idle: ignored.
}

void OtaBleService::_reject_prespawn(OtaStatus status) {
  _notify_status(OtaState::Failed, status);
  if (_on_event) {
    _on_event(OtaState::Failed, status);
  }
}

void OtaBleService::_request_abort(OtaStatus status, bool send_notify) {
  _terminal_status = status;
  _suppress_notify = !send_notify;
  RTOS::task_notify_send(_worker, CMD_ABORT);
}

// ---------------------------------------------------------------------------
// Data characteristic (NimBLE host task)
// ---------------------------------------------------------------------------

void OtaBleService::_on_data_write(const uint8_t *data, size_t len) {
  if (_state == State::Starting) {
    // Phone did not wait for the ready NOTIFY — protocol violation.
    AG_LOGW(TAG, "Data while Starting — protocol violation");
    _request_abort(OtaStatus::TransportError, /*send_notify=*/true);
    return;
  }
  if (_state != State::Downloading) {
    // Data before START or after the terminal edge: ignored.
    return;
  }

  if (data == nullptr || len == 0) {
    AG_LOGW(TAG, "empty Data chunk");
    _request_abort(OtaStatus::TransportError, /*send_notify=*/true);
    return;
  }
  if (len > CONFIG_AG_OTA_BLE_CHUNK_SIZE) {
    AG_LOGW(TAG, "Data chunk %u > buffer %d", static_cast<unsigned>(len),
            CONFIG_AG_OTA_BLE_CHUNK_SIZE);
    _request_abort(OtaStatus::TransportError, /*send_notify=*/true);
    return;
  }
  if (_bytes_accepted + len > _total) {
    AG_LOGW(TAG, "Data overflow past declared size");
    _request_abort(OtaStatus::TransportError, /*send_notify=*/true);
    return;
  }

  // Valid chunk: copy into the single buffer and hand it to the worker. The
  // deferred ACK (blocking here until the worker consumes) is the backpressure.
  std::memcpy(_chunk_buf, data, len);
  _chunk_len = len;
  _bytes_accepted += len;
  RTOS::task_notify_send(_worker, CMD_CHUNK);
  _consumed_sem.take(CONFIG_AG_OTA_BLE_WRITE_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// Worker task (hardware) + directly-invokable steps (host-testable core)
// ---------------------------------------------------------------------------

bool OtaBleService::_spawn_worker() {
#ifndef TEST_HOST
  if (!RTOS::task_create(&_worker_entry, "ota_ble", CONFIG_AG_OTA_BLE_WORKER_STACK_SIZE, this,
                         CONFIG_AG_OTA_BLE_WORKER_PRIORITY, &_worker)) {
    return false;
  }
#endif
  // Host path: no real task; tests pump the steps. The transfer is "active"
  // from here (before begin()) so power management can inhibit sleep.
  _is_active.store(true);
  return true;
}

void OtaBleService::_worker_entry(void *arg) { static_cast<OtaBleService *>(arg)->_worker_loop(); }

void OtaBleService::_worker_loop() {
  if (_begin_step()) {
    bool running = true;
    while (running) {
      uint32_t cmd = 0;
      if (!RTOS::task_notify_wait(&cmd, CONFIG_AG_OTA_BLE_STALL_TIMEOUT_MS)) {
        // Silent-phone stall: the worker is idle and can clean up — normal abort.
        AG_LOGI(TAG, "aborting: no data received (stall)");
        _terminate(OtaStatus::TransportError, /*send_notify=*/true);
        break;
      }
      switch (cmd) {
      case CMD_CHUNK:
        if (!_drain_one()) {
          running = false; // write error — terminal already emitted
        }
        break;
      case CMD_FINISH:
        _finish_step();
        running = false;
        break;
      case CMD_ABORT:
      default:
        _terminate(_terminal_status, !_suppress_notify);
        running = false;
        break;
      }
    }
  }
  _worker_finalize();
}

bool OtaBleService::_begin_step() {
  const OtaStatus st = _writer.begin(_total);
  if (st != OtaStatus::Ok) {
    AG_LOGE(TAG, "writer.begin failed");
    _emit_terminal(OtaState::Failed, OtaStatus::FlashError, /*send_notify=*/true);
    return false;
  }
  _state = State::Downloading;
  AG_LOGI(TAG, "downloading: partition ready, expecting %u bytes", static_cast<unsigned>(_total));
  // The "ready" signal: the phone waits for this NOTIFY before sending Data.
  _emit_event(OtaState::Downloading, OtaStatus::Ok);
  return true;
}

bool OtaBleService::_drain_one() {
  const OtaStatus st = _writer.write(_chunk_buf, _chunk_len);
  if (st != OtaStatus::Ok) {
    AG_LOGE(TAG, "writer.write failed");
    // Release the blocked Data callback first, then abort + announce Failed.
    _consumed_sem.give();
    _writer.abort();
    _emit_terminal(OtaState::Failed, OtaStatus::FlashError, /*send_notify=*/true);
    return false;
  }
  // Release the callback so the phone can send the next chunk.
  _consumed_sem.give();

#ifndef TEST_HOST
  // Throttled progress indication (reuses the pull-path interval knob). Compiled
  // out on host: the log is a no-op there and RTOS has no installed instance.
  const size_t written = _writer.bytes_written();
  const uint64_t now = RTOS::get_time_ms();
  if (now - _last_progress_log_ms >= CONFIG_AG_OTA_PROGRESS_INTERVAL_MS) {
    const unsigned pct = _total > 0 ? static_cast<unsigned>(written * 100 / _total) : 0;
    AG_LOGI(TAG, "progress: %u%% (%u/%u bytes)", pct, static_cast<unsigned>(written),
            static_cast<unsigned>(_total));
    _last_progress_log_ms = now;
  }
#endif
  return true;
}

void OtaBleService::_finish_step() {
  if (_writer.bytes_written() != _total) {
    AG_LOGE(TAG, "END truncated: %u of %u bytes", static_cast<unsigned>(_writer.bytes_written()),
            static_cast<unsigned>(_total));
    _writer.abort();
    _emit_terminal(OtaState::Failed, OtaStatus::TransportError, /*send_notify=*/true);
    return;
  }

  _state = State::Applying;
  AG_LOGI(TAG, "applying: %u bytes received", static_cast<unsigned>(_total));
  _emit_event(OtaState::Applying, OtaStatus::Ok);

  const OtaStatus st = _writer.finish();
  if (st == OtaStatus::Ok) {
    AG_LOGI(TAG, "done: image staged, reboot to run");
    _emit_terminal(OtaState::Done, OtaStatus::Ok, /*send_notify=*/true);
  } else {
    AG_LOGE(TAG, "writer.finish failed");
    _writer.abort();
    _emit_terminal(OtaState::Failed, st, /*send_notify=*/true);
  }
}

void OtaBleService::_terminate(OtaStatus status, bool send_notify) {
  _writer.abort();
  _emit_terminal(OtaState::Failed, status, send_notify);
}

void OtaBleService::_worker_finalize() {
#ifndef TEST_HOST
  // Last act — touch no member afterwards. Signal the teardown waiter, then
  // self-delete; the service is the only safe owner of this task's deletion.
  _worker_exited.give();
  RTOS::task_delete(nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Status emission
// ---------------------------------------------------------------------------

void OtaBleService::_emit_event(OtaState state, OtaStatus status) {
  _notify_status(state, status);
  if (_on_event) {
    _on_event(state, status);
  }
}

void OtaBleService::_emit_terminal(OtaState state, OtaStatus status, bool send_notify) {
  // Release any Data callback still blocked on the handshake.
  _consumed_sem.give();
  if (send_notify) {
    _notify_status(state, status);
  }
  // Cleared BEFORE the terminal on_event so a callback that re-checks
  // is_active() already sees false.
  _is_active.store(false);
  if (_on_event) {
    _on_event(state, status);
  }
  // Terminal owns the single-buffer cleanup and the return to Idle.
  delete[] _chunk_buf;
  _chunk_buf = nullptr;
  _chunk_len = 0;
  _state = State::Idle;
}

void OtaBleService::_notify_status(OtaState state, OtaStatus status) {
  if (_status_char == nullptr) {
    return;
  }

  uint8_t buf[STATUS_CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_STATE);
  cbor_encode_uint(&map, ota_ble_protocol::to_wire(state));
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_RESULT);
  cbor_encode_uint(&map, ota_ble_protocol::to_wire(status));
  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "status encode overflow");
    return;
  }
  const size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  // NOTIFY-only: never set_value(); the Status characteristic stores no value.
  _status_char->notify(buf, len);
}
