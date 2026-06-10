/**
 * AirGradient — OtaBleService host tests
 *
 * Drives the protocol/state core under TEST_HOST against a mock AgBleServer
 * and a fake OtaImageWriter: CBOR Control decode + bounds, the wire-constant
 * mapping, the Idle -> Starting -> Downloading -> Applying state machine, the
 * begin/write/finish sequencing, byte-count/framing rules, NOTIFY-only Status
 * emission, the rejection rules, and the on_event / is_active lifecycle.
 *
 * The FreeRTOS worker task, the live notify/semaphore handshake, and the
 * bounded teardown wait are no-ops under TEST_HOST and are verified by HIL;
 * here the worker steps are driven explicitly (begin_step/drain_one/
 * finish_step/terminate).
 */

#include "services/ota_ble_service.h"
#include "services/ota_ble_protocol.h"

#include "fake_ota_image_writer.h"
#include "mock_ble.h"
#include "ota_ble_service_test_access.h"

#include <cbor.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace {

// Placeholder UUIDs — must match ota_ble_service.cpp.
constexpr const char *OTA_SERVICE_UUID = "ab9a0001-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_CONTROL_CHAR_UUID = "ab9a0002-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_DATA_CHAR_UUID = "ab9a0003-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_STATUS_CHAR_UUID = "ab9a0004-1e3c-4f5a-9b6d-0a1b2c3d4e5f";

// -- CBOR control encoders --------------------------------------------------

std::vector<uint8_t> encode_start(uint64_t total, const char *fw) {
  uint8_t buf[128];
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, fw != nullptr ? 3 : 2);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_OP);
  cbor_encode_text_stringz(&map, OTA_BLE_OP_START);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_TOTAL);
  cbor_encode_uint(&map, total);
  if (fw != nullptr) {
    cbor_encode_text_stringz(&map, OTA_BLE_KEY_FW);
    cbor_encode_text_stringz(&map, fw);
  }
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);
  return std::vector<uint8_t>(buf, buf + len);
}

std::vector<uint8_t> encode_op(const char *op) {
  uint8_t buf[32];
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 1);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_OP);
  cbor_encode_text_stringz(&map, op);
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);
  return std::vector<uint8_t>(buf, buf + len);
}

// Decode a Status NOTIFY payload {state, result} into a pair of wire bytes.
std::pair<uint8_t, uint8_t> decode_status(const std::vector<uint8_t> &payload) {
  CborParser parser;
  CborValue map;
  REQUIRE(cbor_parser_init(payload.data(), payload.size(), 0, &parser, &map) == CborNoError);
  REQUIRE(cbor_value_is_map(&map));

  CborValue v;
  uint64_t state = 0;
  uint64_t result = 0;
  REQUIRE(cbor_value_map_find_value(&map, OTA_BLE_KEY_STATE, &v) == CborNoError);
  REQUIRE(cbor_value_is_unsigned_integer(&v));
  cbor_value_get_uint64(&v, &state);
  REQUIRE(cbor_value_map_find_value(&map, OTA_BLE_KEY_RESULT, &v) == CborNoError);
  REQUIRE(cbor_value_is_unsigned_integer(&v));
  cbor_value_get_uint64(&v, &result);
  return {static_cast<uint8_t>(state), static_cast<uint8_t>(result)};
}

// Test harness bundling the mock server, fake writer, service, captured
// characteristics, and the on_event log.
struct Harness {
  MockBleServer server;
  FakeOtaImageWriter writer;
  OtaBleService svc{server, writer};
  OtaBleServiceTestAccess access{svc};
  MockBleCharacteristic *control = nullptr;
  MockBleCharacteristic *data = nullptr;
  MockBleCharacteristic *status = nullptr;
  std::vector<std::pair<OtaState, OtaStatus>> events;

  Harness() {
    svc.set_on_event([this](OtaState st, OtaStatus res) { events.push_back({st, res}); });
    REQUIRE(svc.setup());
    control = server.find_char(OTA_SERVICE_UUID, OTA_CONTROL_CHAR_UUID);
    data = server.find_char(OTA_SERVICE_UUID, OTA_DATA_CHAR_UUID);
    status = server.find_char(OTA_SERVICE_UUID, OTA_STATUS_CHAR_UUID);
    REQUIRE(control != nullptr);
    REQUIRE(data != nullptr);
    REQUIRE(status != nullptr);
  }

  void write_control(const std::vector<uint8_t> &p) { control->simulate_write(p.data(), p.size()); }
  void write_data(const std::vector<uint8_t> &p) { data->simulate_write(p.data(), p.size()); }
  void write_data(const uint8_t *p, size_t n) { data->simulate_write(p, n); }

  // Forwarders to the friend test-access (private worker steps + internal state).
  bool begin_step() { return access.begin_step(); }
  bool drain_one() { return access.drain_one(); }
  void finish_step() { access.finish_step(); }
  void terminate(OtaStatus status, bool send_notify = true) {
    access.terminate(status, send_notify);
  }
  uint8_t internal_state() const { return access.internal_state(); }
  OtaStatus pending_terminal_status() const { return access.pending_terminal_status(); }
  bool pending_suppress_notify() const { return access.pending_suppress_notify(); }
};

// Internal-state wire values from OtaBleService::State.
constexpr uint8_t ST_IDLE = 0;
constexpr uint8_t ST_STARTING = 1;
constexpr uint8_t ST_DOWNLOADING = 2;
constexpr uint8_t ST_APPLYING = 3;

} // namespace

// ===========================================================================
// setup / GATT registration
// ===========================================================================

TEST_CASE("setup registers OTA service with Control/Data/Status characteristics") {
  Harness h;
  MockBleGattService *svc = h.server.find_service(OTA_SERVICE_UUID);
  REQUIRE(svc != nullptr);
  REQUIRE(svc->started);

  // Control/Data: WRITE | WRITE_AUTHEN. Status: NOTIFY | READ_AUTHEN (no READ).
  REQUIRE(svc->props_of(OTA_CONTROL_CHAR_UUID) ==
          (AgBleProperty::WRITE | AgBleProperty::WRITE_AUTHEN));
  REQUIRE(svc->props_of(OTA_DATA_CHAR_UUID) ==
          (AgBleProperty::WRITE | AgBleProperty::WRITE_AUTHEN));
  REQUIRE(svc->props_of(OTA_STATUS_CHAR_UUID) ==
          (AgBleProperty::NOTIFY | AgBleProperty::READ_AUTHEN));
  REQUIRE((svc->props_of(OTA_STATUS_CHAR_UUID) & AgBleProperty::READ) == 0);

  REQUIRE(h.control->has_write_callback());
  REQUIRE(h.data->has_write_callback());
}

TEST_CASE("setup returns false when the service cannot be added") {
  MockBleServer server;
  server.add_service_returns = false;
  FakeOtaImageWriter writer;
  OtaBleService svc{server, writer};
  REQUIRE_FALSE(svc.setup());
}

// ===========================================================================
// Wire constants
// ===========================================================================

TEST_CASE("to_wire pins the frozen state/result bytes") {
  using namespace ota_ble_protocol;
  REQUIRE(to_wire(OtaState::Downloading) == 0x01);
  REQUIRE(to_wire(OtaState::Applying) == 0x02);
  REQUIRE(to_wire(OtaState::Done) == 0x03);
  REQUIRE(to_wire(OtaState::Failed) == 0x04);

  REQUIRE(to_wire(OtaStatus::Ok) == 0x00);
  REQUIRE(to_wire(OtaStatus::FlashError) == 0x01);
  REQUIRE(to_wire(OtaStatus::InvalidImage) == 0x02);
  REQUIRE(to_wire(OtaStatus::TransportError) == 0x03);
  REQUIRE(to_wire(OtaStatus::Aborted) == 0x04);
  REQUIRE(to_wire(OtaStatus::InvalidArgument) == 0x05);
}

// ===========================================================================
// Happy path / sequencing
// ===========================================================================

TEST_CASE("valid START spawns the worker and stays Starting until begin runs") {
  Harness h;
  h.write_control(encode_start(1024, "3.2.0"));

  REQUIRE(h.internal_state() == ST_STARTING);
  REQUIRE(h.svc.is_active());           // set at spawn, before begin()
  REQUIRE_FALSE(h.writer.begin_called); // begin runs on the worker, not here
  REQUIRE(h.status->notify_count == 0);

  REQUIRE(h.begin_step());
  REQUIRE(h.writer.begin_called);
  REQUIRE(h.writer.begin_total == 1024);
  REQUIRE(h.internal_state() == ST_DOWNLOADING);

  // The ready signal: NOTIFY Downloading{Ok} + on_event.
  REQUIRE(h.status->notify_count == 1);
  REQUIRE(decode_status(h.status->all_notified.back()) ==
          std::make_pair<uint8_t, uint8_t>(0x01, 0x00));
  REQUIRE(h.events.size() == 1);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Downloading, OtaStatus::Ok));
}

TEST_CASE("full transfer: START -> chunks -> END -> Done") {
  Harness h;
  h.write_control(encode_start(6, "fw"));
  REQUIRE(h.begin_step());

  std::vector<uint8_t> c1{1, 2, 3};
  std::vector<uint8_t> c2{4, 5, 6};
  h.write_data(c1);
  REQUIRE(h.drain_one());
  h.write_data(c2);
  REQUIRE(h.drain_one());

  REQUIRE(h.writer.write_calls == 2);
  REQUIRE(h.writer.bytes_written() == 6);

  // No progress NOTIFY between ready and Applying.
  REQUIRE(h.status->notify_count == 1);

  h.write_control(encode_op(OTA_BLE_OP_END));
  REQUIRE(h.internal_state() == ST_DOWNLOADING); // END signals the worker
  h.finish_step();

  REQUIRE(h.writer.finish_called);
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.svc.is_active());

  // Applying then Done NOTIFY + events.
  REQUIRE(h.status->notify_count == 3);
  REQUIRE(decode_status(h.status->all_notified[1]) ==
          std::make_pair<uint8_t, uint8_t>(0x02, 0x00)); // Applying
  REQUIRE(decode_status(h.status->all_notified[2]) ==
          std::make_pair<uint8_t, uint8_t>(0x03, 0x00)); // Done
  REQUIRE(h.events.size() == 3);
  REQUIRE(h.events[0] == std::make_pair(OtaState::Downloading, OtaStatus::Ok));
  REQUIRE(h.events[1] == std::make_pair(OtaState::Applying, OtaStatus::Ok));
  REQUIRE(h.events[2] == std::make_pair(OtaState::Done, OtaStatus::Ok));
}

TEST_CASE("Status is NOTIFY-only — never sets a stored READ value") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.begin_step());
  std::vector<uint8_t> c{7, 8, 9};
  h.write_data(c);
  REQUIRE(h.drain_one());
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  REQUIRE(h.status->set_value_count == 0);
  REQUIRE(h.status->notify_count == 3);
}

// ===========================================================================
// Control decode + bounds
// ===========================================================================

TEST_CASE("malformed control CBOR while Idle is rejected InvalidArgument") {
  Harness h;
  std::vector<uint8_t> garbage{0xFF, 0xFF, 0xFF};
  h.write_control(garbage);

  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE_FALSE(h.writer.begin_called);
  REQUIRE(h.events.size() == 1);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::InvalidArgument));
  REQUIRE(decode_status(h.status->all_notified.back()) ==
          std::make_pair<uint8_t, uint8_t>(0x04, 0x05));
}

TEST_CASE("oversized control write is rejected InvalidArgument") {
  Harness h;
  std::vector<uint8_t> big(128, 0x20); // > CONTROL_MAX_BYTES (64)
  h.write_control(big);
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::InvalidArgument));
}

TEST_CASE("START with total=0 is rejected InvalidArgument") {
  Harness h;
  h.write_control(encode_start(0, "fw"));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.writer.begin_called);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::InvalidArgument));
}

TEST_CASE("START with an over-long fw string is rejected InvalidArgument") {
  Harness h;
  std::string long_fw(64, 'x'); // > FW_MAX_LEN (32)
  h.write_control(encode_start(1024, long_fw.c_str()));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::InvalidArgument));
}

TEST_CASE("START without fw is accepted (fw is informational)") {
  Harness h;
  h.write_control(encode_start(512, nullptr));
  REQUIRE(h.internal_state() == ST_STARTING);
  REQUIRE(h.svc.is_active());
}

// ===========================================================================
// Byte-count / framing rules
// ===========================================================================

TEST_CASE("oversized Data chunk records a TransportError abort") {
  Harness h;
  h.write_control(encode_start(4096, nullptr));
  REQUIRE(h.begin_step());

  std::vector<uint8_t> big(1024, 0xAB); // > CHUNK_SIZE (512)
  h.write_data(big);
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
  REQUIRE_FALSE(h.pending_suppress_notify());
  REQUIRE(h.writer.write_calls == 0); // never copied/written

  h.terminate(h.pending_terminal_status());
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::TransportError));
  REQUIRE(h.internal_state() == ST_IDLE);
}

TEST_CASE("empty Data while Downloading records a TransportError abort") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.begin_step());
  h.write_data(nullptr, 0);
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
}

TEST_CASE("Data overflow past the declared total records a TransportError abort") {
  Harness h;
  h.write_control(encode_start(4, nullptr)); // total = 4
  REQUIRE(h.begin_step());
  std::vector<uint8_t> chunk{1, 2, 3, 4, 5}; // 5 > 4
  h.write_data(chunk);
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
  REQUIRE(h.writer.write_calls == 0);
}

TEST_CASE("early END (truncated) aborts with TransportError and never finishes") {
  Harness h;
  h.write_control(encode_start(10, nullptr));
  REQUIRE(h.begin_step());
  std::vector<uint8_t> chunk{1, 2, 3}; // only 3 of 10
  h.write_data(chunk);
  REQUIRE(h.drain_one());

  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step(); // bytes_written (3) != total (10)

  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::TransportError));
  REQUIRE(h.internal_state() == ST_IDLE);
}

TEST_CASE("Data while Starting (before the ready NOTIFY) is a protocol violation") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.internal_state() == ST_STARTING);

  std::vector<uint8_t> chunk{1, 2, 3};
  h.write_data(chunk);
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
  REQUIRE_FALSE(h.pending_suppress_notify());
  REQUIRE(h.writer.write_calls == 0);
}

// ===========================================================================
// Chunk-write error
// ===========================================================================

TEST_CASE("a writer.write failure emits exactly one Failed{FlashError}") {
  Harness h;
  h.write_control(encode_start(8, nullptr));
  REQUIRE(h.begin_step());

  h.writer.write_status = OtaStatus::FlashError;
  std::vector<uint8_t> chunk{1, 2, 3, 4};
  h.write_data(chunk);
  REQUIRE_FALSE(h.drain_one());

  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.svc.is_active());

  // Exactly one Failed NOTIFY (ready + Failed = 2 total).
  REQUIRE(h.status->notify_count == 2);
  REQUIRE(decode_status(h.status->all_notified.back()) ==
          std::make_pair<uint8_t, uint8_t>(0x04, 0x01));
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::FlashError));
}

// ===========================================================================
// begin / finish flash failures
// ===========================================================================

TEST_CASE("writer.begin failure terminates with FlashError") {
  Harness h;
  h.writer.begin_status = OtaStatus::FlashError;
  h.write_control(encode_start(64, nullptr));
  REQUIRE_FALSE(h.begin_step());

  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::FlashError));
}

TEST_CASE("finish validation failure maps to InvalidImage") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.begin_step());
  std::vector<uint8_t> chunk{1, 2, 3};
  h.write_data(chunk);
  REQUIRE(h.drain_one());

  h.writer.finish_status = OtaStatus::InvalidImage;
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  REQUIRE(h.writer.finish_called);
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::InvalidImage));
}

// ===========================================================================
// Failure mapping: ABORT / disconnect
// ===========================================================================

TEST_CASE("Control ABORT maps to Aborted with a NOTIFY") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.begin_step());

  h.write_control(encode_op(OTA_BLE_OP_ABORT));
  REQUIRE(h.pending_terminal_status() == OtaStatus::Aborted);
  REQUIRE_FALSE(h.pending_suppress_notify());

  h.terminate(h.pending_terminal_status(), !h.pending_suppress_notify());
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::Aborted));
  REQUIRE(decode_status(h.status->all_notified.back()) ==
          std::make_pair<uint8_t, uint8_t>(0x04, 0x04));
}

TEST_CASE("disconnect maps to TransportError and suppresses the NOTIFY") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.begin_step());
  const int notifies_before = h.status->notify_count;

  h.svc.handle_disconnect();
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
  REQUIRE(h.pending_suppress_notify());

  h.terminate(h.pending_terminal_status(), !h.pending_suppress_notify());
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.events.back() == std::make_pair(OtaState::Failed, OtaStatus::TransportError));
  REQUIRE(h.status->notify_count == notifies_before); // no NOTIFY on disconnect
}

TEST_CASE("disconnect while idle is a no-op") {
  Harness h;
  h.svc.handle_disconnect();
  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE(h.events.empty());
}

// ===========================================================================
// Rejection rules
// ===========================================================================

TEST_CASE("Data before START is ignored") {
  Harness h;
  std::vector<uint8_t> chunk{1, 2, 3};
  h.write_data(chunk);
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.writer.write_calls == 0);
  REQUIRE(h.events.empty());
}

TEST_CASE("a second START while active is rejected (no second worker)") {
  Harness h;
  h.write_control(encode_start(64, "a"));
  REQUIRE(h.begin_step());
  const size_t events_before = h.events.size();

  h.write_control(encode_start(128, "b"));
  REQUIRE(h.internal_state() == ST_DOWNLOADING);
  REQUIRE(h.writer.begin_total == 64);       // unchanged
  REQUIRE(h.events.size() == events_before); // no new event
}

TEST_CASE("END while Idle is ignored") {
  Harness h;
  h.write_control(encode_op(OTA_BLE_OP_END));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE(h.events.empty());
}

TEST_CASE("ABORT while Idle is ignored") {
  Harness h;
  h.write_control(encode_op(OTA_BLE_OP_ABORT));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.events.empty());
}

// ===========================================================================
// is_active lifecycle
// ===========================================================================

TEST_CASE("is_active is false before START, true mid-transfer, false in the terminal event") {
  Harness h;
  REQUIRE_FALSE(h.svc.is_active());

  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.svc.is_active());
  REQUIRE(h.begin_step());
  REQUIRE(h.svc.is_active());

  // The terminal on_event must already observe is_active() == false.
  bool active_in_terminal = true;
  h.svc.set_on_event([&](OtaState st, OtaStatus res) {
    h.events.push_back({st, res});
    if (st == OtaState::Done || st == OtaState::Failed) {
      active_in_terminal = h.svc.is_active();
    }
  });

  std::vector<uint8_t> chunk{1, 2, 3};
  h.write_data(chunk);
  REQUIRE(h.drain_one());
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE_FALSE(active_in_terminal);
}

TEST_CASE("a fresh START after a completed transfer is accepted") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.begin_step());
  std::vector<uint8_t> chunk{1, 2, 3};
  h.write_data(chunk);
  REQUIRE(h.drain_one());
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();
  REQUIRE(h.internal_state() == ST_IDLE);

  // Second transfer.
  h.write_control(encode_start(2, nullptr));
  REQUIRE(h.internal_state() == ST_STARTING);
  REQUIRE(h.begin_step());
  REQUIRE(h.writer.begin_total == 2);
}
