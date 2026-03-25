/**
 * AirGradient Go — BLE Service tests
 *
 * Tests CBOR encoding, wire format conversion, pending write buffers,
 * notification flow, connection lifecycle, history download, and string
 * mapping through the BleServiceTestAccess friend class.
 */

#include "go_ble.h"
#include "go_storage.h"
#include "hal/ble_server.h"

#include <cbor.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>
#include <vector>

// ===========================================================================
// Mock BLE types
// ===========================================================================

/// Captures set_value() data and notify() calls for test assertions.
class MockBleCharacteristic : public AgBleCharacteristic {
public:
  bool set_value(const uint8_t *data, size_t len) override {
    last_value.assign(data, data + len);
    set_value_count++;
    return true;
  }

  bool notify() override {
    notify_count++;
    return notify_returns;
  }

  void set_write_callback(AgBleWriteCallback callback) override { write_cb = callback; }

  // --- Test inspection ---
  std::vector<uint8_t> last_value;
  int set_value_count = 0;
  int notify_count = 0;
  bool notify_returns = true;
  AgBleWriteCallback write_cb;

  void reset() {
    last_value.clear();
    set_value_count = 0;
    notify_count = 0;
    notify_returns = true;
  }
};

/// Captures advertising calls for connection lifecycle tests.
class MockBleServer : public AgBleServer {
public:
  bool init(const char * /*device_name*/) override { return true; }
  void deinit() override {}
  bool set_security(AgBleIoCapability /*io_cap*/, uint8_t /*auth_flags*/) override { return true; }
  bool delete_all_bonds() override {
    delete_all_bonds_count++;
    return delete_all_bonds_result;
  }
  AgBleGattService *add_service(const char * /*uuid*/) override { return nullptr; }
  bool set_advertising_name(const char * /*name*/) override { return true; }
  bool add_advertised_service_uuid(const char * /*uuid*/) override { return true; }

  bool start_advertising() override {
    start_advertising_count++;
    return true;
  }

  bool stop_advertising() override {
    stop_advertising_count++;
    return true;
  }

  void set_connect_callback(AgBleConnectCallback /*cb*/) override {}
  void set_disconnect_callback(AgBleDisconnectCallback /*cb*/) override {}
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback /*cb*/) override {}
  void set_auth_complete_callback(AgBleAuthCompleteCallback /*cb*/) override {}

  // --- Test inspection ---
  int start_advertising_count = 0;
  int stop_advertising_count = 0;
  int delete_all_bonds_count = 0;
  bool delete_all_bonds_result = true;

  void reset() {
    start_advertising_count = 0;
    stop_advertising_count = 0;
    delete_all_bonds_count = 0;
    delete_all_bonds_result = true;
  }
};

// ===========================================================================
// Storage spy — controllable stub data for history tests
// ===========================================================================

namespace storage_spy {

struct SessionEntry {
  uint32_t id;
  uint32_t point_count;
  time_t start_time;
};

static std::vector<SessionEntry> sessions;
static std::vector<RoutePoint> points;
static uint32_t total_capacity_kb = 0;
static uint32_t used_kb = 0;

static void reset() {
  sessions.clear();
  points.clear();
  total_capacity_kb = 0;
  used_kb = 0;
}

} // namespace storage_spy

// ===========================================================================
// StorageService stubs (link-time replacements)
// ===========================================================================

// Stubs for methods that go_ble.cpp doesn't call but must link
StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}
bool StorageService::init() { return true; }
void StorageService::cache_measurement(const MeasuresAGo & /*m*/) {}
void StorageService::clear_cache() {}
uint16_t StorageService::read_cached_field(CacheField /*f*/, float * /*out*/,
                                           uint16_t /*max*/) const {
  return 0;
}
uint16_t StorageService::read_cache(MeasuresAGo * /*out*/, uint16_t /*max*/) const { return 0; }
uint16_t StorageService::cached_count() const { return 0; }
void StorageService::backup_cache() const {}
void StorageService::restore_cache() {}
bool StorageService::start_route(uint32_t /*session_id*/) { return true; }
bool StorageService::append_route_point(const RoutePoint & /*point*/) { return true; }
void StorageService::end_route() {}
bool StorageService::is_route_active() const { return false; }
uint32_t StorageService::current_route_point_count() const { return 0; }
bool StorageService::clear_routes() { return true; }
bool StorageService::ensure_route_dir() const { return true; }

// History read stubs — controlled by storage_spy
uint16_t StorageService::list_sessions(uint32_t *out, uint16_t max_count) const {
  uint16_t count = 0;
  for (size_t i = 0; i < storage_spy::sessions.size() && count < max_count; i++) {
    out[count++] = storage_spy::sessions[i].id;
  }
  return count;
}

uint32_t StorageService::get_session_point_count(uint32_t session_id) const {
  for (const auto &s : storage_spy::sessions) {
    if (s.id == session_id) {
      return s.point_count;
    }
  }
  return 0;
}

uint16_t StorageService::read_route_points(uint32_t /*session_id*/, uint32_t offset,
                                           RoutePoint *out, uint16_t count) const {
  uint16_t read = 0;
  for (uint16_t i = 0; i < count && (offset + i) < storage_spy::points.size(); i++) {
    out[i] = storage_spy::points[offset + i];
    read++;
  }
  return read;
}

time_t StorageService::get_session_start_time(uint32_t session_id) const {
  for (const auto &s : storage_spy::sessions) {
    if (s.id == session_id) {
      return s.start_time;
    }
  }
  return 0;
}

uint32_t StorageService::total_capacity_kb() const { return storage_spy::total_capacity_kb; }

uint32_t StorageService::used_kb() const { return storage_spy::used_kb; }

// ===========================================================================
// BleServiceTestAccess — friend class for private member access
// ===========================================================================

class BleServiceTestAccess {
public:
  // --- State setters ---
  static void set_server(BleService &svc, AgBleServer *server) { svc._server = server; }
  static void set_connected(BleService &svc, bool connected) { svc._connected.store(connected); }
  static void set_measures_char(BleService &svc, AgBleCharacteristic *c) { svc._measures_char = c; }
  static void set_status_char(BleService &svc, AgBleCharacteristic *c) { svc._status_char = c; }
  static void set_config_char(BleService &svc, AgBleCharacteristic *c) { svc._config_char = c; }
  static void set_history_char(BleService &svc, AgBleCharacteristic *c) { svc._history_char = c; }
  static void set_export_active(BleService &svc, bool active) { svc._export_active = active; }
  static void set_export_session_id(BleService &svc, uint32_t id) { svc._export_session_id = id; }

  // --- Private method access ---
  static void on_connect(BleService &svc, uint16_t h) { svc.on_connect(h); }
  static void on_disconnect(BleService &svc, uint16_t h, int r) { svc.on_disconnect(h, r); }
  static void on_config_write(BleService &svc, const uint8_t *d, size_t l) {
    svc.on_config_write(d, l);
  }
  static void on_history_write(BleService &svc, const uint8_t *d, size_t l) {
    svc.on_history_write(d, l);
  }

  static size_t encode_measures(BleService &svc, uint8_t *buf, size_t sz, const MeasuresAGo &m,
                                const GpsData &gps, time_t ts) {
    return svc.encode_measures(buf, sz, m, gps, ts);
  }
  static size_t encode_status(BleService &svc, uint8_t *buf, size_t sz, const PowerSnapshot &p,
                              const GpsData &gps, bool tracking, uint32_t session_id) {
    return svc.encode_status(buf, sz, p, gps, tracking, session_id);
  }
  static size_t encode_config(BleService &svc, uint8_t *buf, size_t sz, const GoSettings &s) {
    return svc.encode_config(buf, sz, s);
  }

  static void route_point_to_wire(const RoutePoint &point, uint8_t *out) {
    BleService::route_point_to_wire(point, out);
  }

  static const char *charging_state_to_str(BmsChargingState state) {
    return BleService::charging_state_to_str(state);
  }
  static const char *gps_mode_to_str(GpsMode mode) { return BleService::gps_mode_to_str(mode); }
  static const char *operating_mode_to_str(OperatingMode mode) {
    return BleService::operating_mode_to_str(mode);
  }

  // --- State readers ---
  static bool export_active(const BleService &svc) { return svc._export_active; }
  static uint32_t export_session_id(const BleService &svc) { return svc._export_session_id; }
};

// ===========================================================================
// CBOR decode helpers for test assertions
// ===========================================================================

/// Decode a CBOR map from a buffer. Returns key-value pairs as string->CborValue.
/// Only supports top-level flat maps. Asserts on decode errors.
struct CborMapEntry {
  std::string key;
  CborType type;
  // Decoded value — use whichever field matches .type
  int64_t int_val = 0;
  uint64_t uint_val = 0;
  double float_val = 0.0;
  bool bool_val = false;
  std::string text_val;
};

static std::vector<CborMapEntry> decode_cbor_map(const uint8_t *data, size_t len) {
  std::vector<CborMapEntry> entries;
  CborParser parser;
  CborValue it;
  REQUIRE(cbor_parser_init(data, len, 0, &parser, &it) == CborNoError);
  REQUIRE(cbor_value_is_map(&it));

  CborValue map;
  REQUIRE(cbor_value_enter_container(&it, &map) == CborNoError);

  while (!cbor_value_at_end(&map)) {
    CborMapEntry entry;

    // Key must be text
    REQUIRE(cbor_value_is_text_string(&map));
    size_t key_len = 0;
    cbor_value_get_string_length(&map, &key_len);
    entry.key.resize(key_len);
    cbor_value_copy_text_string(&map, entry.key.data(), &key_len, &map);

    // Value
    entry.type = cbor_value_get_type(&map);
    if (cbor_value_is_unsigned_integer(&map)) {
      cbor_value_get_uint64(&map, &entry.uint_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_integer(&map)) {
      cbor_value_get_int64(&map, &entry.int_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_float(&map)) {
      float f;
      cbor_value_get_float(&map, &f);
      entry.float_val = static_cast<double>(f);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_double(&map)) {
      cbor_value_get_double(&map, &entry.float_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_boolean(&map)) {
      cbor_value_get_boolean(&map, &entry.bool_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_text_string(&map)) {
      size_t val_len = 0;
      cbor_value_get_string_length(&map, &val_len);
      entry.text_val.resize(val_len);
      cbor_value_copy_text_string(&map, entry.text_val.data(), &val_len, &map);
    } else {
      // Skip unknown types
      cbor_value_advance(&map);
    }

    entries.push_back(entry);
  }

  return entries;
}

static const CborMapEntry *find_entry(const std::vector<CborMapEntry> &entries,
                                      const std::string &key) {
  for (const auto &e : entries) {
    if (e.key == key) {
      return &e;
    }
  }
  return nullptr;
}

// ===========================================================================
// Test fixture helpers
// ===========================================================================

/// Null-backed PayloadCache and NandStorage for StorageService construction.
/// StorageService methods are all stubbed, so these are never used.
static PayloadCache *null_cache_ptr = nullptr;
static NandStorage *null_nand_ptr = nullptr;

/// Helper: constructs a valid MeasuresAGo with all sensors reading valid data.
static MeasuresAGo make_valid_measures() {
  MeasuresAGo m{};
  m.temp_hum_a.temperature = 23.5f;
  m.temp_hum_a.humidity = 45.2f;
  m.pm_a.pm_01 = 5.0f;
  m.pm_a.pm_25 = 8.3f;
  m.pm_a.pm_10 = 12.1f;
  m.co2.co2 = 450;
  m.tvoc_nox.tvoc_index = 120;
  m.tvoc_nox.nox_index = 5;
  m.pressure.pressure = 1013.2f;
  return m;
}

/// Helper: constructs a valid GpsData with a 3D fix.
static GpsData make_valid_gps() {
  GpsData gps{};
  gps.position.latitude = 47.376887;
  gps.position.longitude = 8.541694;
  gps.altitude_m = 408.0f;
  gps.fix.fix_type = GpsFixType::Fix3D;
  gps.fix.satellite_count = 12;
  return gps;
}

/// Helper: constructs default GoSettings.
static GoSettings make_default_settings() { return GoSettings{}; }

/// Helper: constructs a PowerSnapshot with valid data.
static PowerSnapshot make_valid_power() {
  PowerSnapshot p{};
  p.battery_voltage = 3.85f;
  p.battery_percentage = 72.0f;
  p.charging_status = BmsChargingState::NotCharging;
  return p;
}

// ===========================================================================
// Tests
// ===========================================================================

TEST_CASE("BLE: state queries default values") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  CHECK(svc.is_initialized() == false);
  CHECK(svc.is_connected() == false);
}

// ---------------------------------------------------------------------------
// Pending write buffers
// ---------------------------------------------------------------------------

TEST_CASE("BLE: take_pending_config_write returns 0 when no data pending") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  uint8_t buf[256];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write stores data and take retrieves it") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  uint8_t write_data[] = {0xA1, 0x62, 0x6F, 0x70};
  BleServiceTestAccess::on_config_write(svc, write_data, sizeof(write_data));

  uint8_t buf[256];
  size_t len = svc.take_pending_config_write(buf, sizeof(buf));
  REQUIRE(len == sizeof(write_data));
  CHECK(memcmp(buf, write_data, len) == 0);

  // Second call returns 0 (cleared)
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write rejects null data") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  BleServiceTestAccess::on_config_write(svc, nullptr, 4);

  uint8_t buf[256];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write rejects zero length") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  uint8_t data[] = {0x01};
  BleServiceTestAccess::on_config_write(svc, data, 0);

  uint8_t buf[256];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write rejects oversized data") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  uint8_t data[257];
  memset(data, 0xAA, sizeof(data));
  BleServiceTestAccess::on_config_write(svc, data, sizeof(data));

  uint8_t buf[257];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: take_pending_config_write truncates to buffer size") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  uint8_t write_data[16];
  for (size_t i = 0; i < sizeof(write_data); i++) {
    write_data[i] = static_cast<uint8_t>(i);
  }
  BleServiceTestAccess::on_config_write(svc, write_data, sizeof(write_data));

  uint8_t buf[4];
  size_t len = svc.take_pending_config_write(buf, sizeof(buf));
  REQUIRE(len == 4);
  CHECK(buf[0] == 0x00);
  CHECK(buf[3] == 0x03);
}

TEST_CASE("BLE: on_history_write stores data and take retrieves it") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  uint8_t write_data[] = {0xB1, 0x22, 0x33};
  BleServiceTestAccess::on_history_write(svc, write_data, sizeof(write_data));

  uint8_t buf[256];
  size_t len = svc.take_pending_history_write(buf, sizeof(buf));
  REQUIRE(len == sizeof(write_data));
  CHECK(memcmp(buf, write_data, len) == 0);

  // Cleared after retrieval
  CHECK(svc.take_pending_history_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_history_write rejects invalid input") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  // null data
  BleServiceTestAccess::on_history_write(svc, nullptr, 4);
  uint8_t buf[256];
  CHECK(svc.take_pending_history_write(buf, sizeof(buf)) == 0);

  // zero length
  uint8_t data[] = {0x01};
  BleServiceTestAccess::on_history_write(svc, data, 0);
  CHECK(svc.take_pending_history_write(buf, sizeof(buf)) == 0);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Measures
// ---------------------------------------------------------------------------

TEST_CASE("BLE: encode_measures with all sensors valid and GPS fix") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  auto m = make_valid_measures();
  auto gps = make_valid_gps();
  time_t ts = 1711234567;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);

  // All sensor fields present
  CHECK(find_entry(entries, "t") != nullptr);
  CHECK(find_entry(entries, "h") != nullptr);
  CHECK(find_entry(entries, "pm1") != nullptr);
  CHECK(find_entry(entries, "pm25") != nullptr);
  CHECK(find_entry(entries, "pm10") != nullptr);
  CHECK(find_entry(entries, "co2") != nullptr);
  CHECK(find_entry(entries, "tvoc") != nullptr);
  CHECK(find_entry(entries, "nox") != nullptr);
  CHECK(find_entry(entries, "pres") != nullptr);

  // GPS fields present
  CHECK(find_entry(entries, "lat") != nullptr);
  CHECK(find_entry(entries, "lon") != nullptr);
  CHECK(find_entry(entries, "alt") != nullptr);
  CHECK(find_entry(entries, "fix") != nullptr);
  CHECK(find_entry(entries, "sat") != nullptr);

  // Timestamp always present
  auto *ts_entry = find_entry(entries, "ts");
  REQUIRE(ts_entry != nullptr);
  CHECK(ts_entry->uint_val == 1711234567);

  // Spot-check values
  CHECK_THAT(find_entry(entries, "t")->float_val, Catch::Matchers::WithinAbs(23.5, 0.1));
  CHECK(find_entry(entries, "co2")->uint_val == 450);
  CHECK_THAT(find_entry(entries, "lat")->float_val, Catch::Matchers::WithinAbs(47.376887, 0.0001));
  CHECK(find_entry(entries, "fix")->uint_val == 3);
  CHECK(find_entry(entries, "sat")->uint_val == 12);
}

TEST_CASE("BLE: encode_measures without GPS fix omits GPS keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  auto m = make_valid_measures();
  GpsData gps{}; // default = NoFix
  time_t ts = 1711234567;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);

  CHECK(find_entry(entries, "lat") == nullptr);
  CHECK(find_entry(entries, "lon") == nullptr);
  CHECK(find_entry(entries, "alt") == nullptr);
  CHECK(find_entry(entries, "fix") == nullptr);
  CHECK(find_entry(entries, "sat") == nullptr);

  // Sensor fields still present
  CHECK(find_entry(entries, "t") != nullptr);
  CHECK(find_entry(entries, "ts") != nullptr);
}

TEST_CASE("BLE: encode_measures omits invalid sensor fields") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  MeasuresAGo m{};
  // Only temperature is valid
  m.temp_hum_a.temperature = 22.0f;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.co2.co2 = MeasuresInvalid::CO2;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.pressure.pressure = MeasuresInvalid::PRESSURE;

  GpsData gps{};
  time_t ts = 100;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);

  CHECK(find_entry(entries, "t") != nullptr);
  CHECK(find_entry(entries, "h") == nullptr);
  CHECK(find_entry(entries, "co2") == nullptr);
  CHECK(find_entry(entries, "tvoc") == nullptr);
  CHECK(find_entry(entries, "nox") == nullptr);
  CHECK(find_entry(entries, "pres") == nullptr);
  CHECK(find_entry(entries, "ts") != nullptr);
}

TEST_CASE("BLE: encode_measures with no valid sensors has only timestamp") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  MeasuresAGo m{};
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.pm_a.pm_01 = MeasuresInvalid::PM;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.pm_a.pm_10 = MeasuresInvalid::PM;
  m.co2.co2 = MeasuresInvalid::CO2;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.pressure.pressure = MeasuresInvalid::PRESSURE;

  GpsData gps{};
  time_t ts = 999;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 1);
  CHECK(entries[0].key == "ts");
  CHECK(entries[0].uint_val == 999);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Status
// ---------------------------------------------------------------------------

TEST_CASE("BLE: encode_status has all 10 keys") {
  storage_spy::reset();
  storage_spy::total_capacity_kb = 262144;
  storage_spy::used_kb = 8192;

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  auto power = make_valid_power();
  auto gps = make_valid_gps();

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_status(svc, buf, sizeof(buf), power, gps, true, 10042);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 10);

  CHECK(find_entry(entries, "gps_fix")->uint_val == 3);
  CHECK(find_entry(entries, "gps_sat")->uint_val == 12);
  CHECK(find_entry(entries, "bat_pct")->uint_val == 72);
  CHECK_THAT(find_entry(entries, "bat_v")->float_val, Catch::Matchers::WithinAbs(3.85, 0.01));
  CHECK(find_entry(entries, "charging")->text_val == "none");
  CHECK(find_entry(entries, "tracking")->bool_val == true);
  CHECK(find_entry(entries, "session")->uint_val == 10042);
  CHECK(find_entry(entries, "flash_kb")->uint_val == 262144);
  CHECK(find_entry(entries, "used_kb")->uint_val == 8192);
  CHECK(find_entry(entries, "fw")->text_val == "unknown");
}

TEST_CASE("BLE: encode_status clamps negative battery values to 0") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);

  PowerSnapshot power{};
  power.battery_voltage = -1.0f;
  power.battery_percentage = -1.0f;
  GpsData gps{};

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_status(svc, buf, sizeof(buf), power, gps, false, 0);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(find_entry(entries, "bat_pct")->uint_val == 0);
  CHECK_THAT(find_entry(entries, "bat_v")->float_val, Catch::Matchers::WithinAbs(0.0, 0.01));
}

// ---------------------------------------------------------------------------
// CBOR encoding: Config
// ---------------------------------------------------------------------------

TEST_CASE("BLE: encode_config produces 12 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  auto settings = make_default_settings();

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_config(svc, buf, sizeof(buf), settings);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 12);

  CHECK(find_entry(entries, "meas_int") != nullptr);
  CHECK(find_entry(entries, "pm_int") != nullptr);
  CHECK(find_entry(entries, "other_int") != nullptr);
  CHECK(find_entry(entries, "disp_int") != nullptr);
  CHECK(find_entry(entries, "temp_f") != nullptr);
  CHECK(find_entry(entries, "pm_aqi") != nullptr);
  CHECK(find_entry(entries, "gps_int") != nullptr);
  CHECK(find_entry(entries, "gps_mode") != nullptr);
  CHECK(find_entry(entries, "inact_to") != nullptr);
  CHECK(find_entry(entries, "auto_lock") != nullptr);
  CHECK(find_entry(entries, "dev_name") != nullptr);
  CHECK(find_entry(entries, "op_mode") != nullptr);
}

TEST_CASE("BLE: encode_config values match settings") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  GoSettings s{};
  s.measurement_interval_seconds = 30;
  s.use_fahrenheit = true;
  s.gps_mode = GpsMode::AlwaysOn;
  s.device_name = "test-device";
  s.operating_mode = OperatingMode::Stationary;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_config(svc, buf, sizeof(buf), s);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(find_entry(entries, "meas_int")->uint_val == 30);
  CHECK(find_entry(entries, "temp_f")->bool_val == true);
  CHECK(find_entry(entries, "gps_mode")->text_val == "always");
  CHECK(find_entry(entries, "dev_name")->text_val == "test-device");
  CHECK(find_entry(entries, "op_mode")->text_val == "stationary");
}

// ---------------------------------------------------------------------------
// notify_config (13 keys: 12 config + type discriminator)
// ---------------------------------------------------------------------------

TEST_CASE("BLE: notify_config produces 13 keys with type discriminator") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  auto settings = make_default_settings();
  svc.notify_config(settings);

  REQUIRE(config_char.set_value_count == 1);
  REQUIRE(config_char.notify_count == 1);

  auto entries = decode_cbor_map(config_char.last_value.data(), config_char.last_value.size());
  CHECK(entries.size() == 13);

  auto *type_entry = find_entry(entries, "type");
  REQUIRE(type_entry != nullptr);
  CHECK(type_entry->text_val == "config");
}

// ---------------------------------------------------------------------------
// notify_command_result
// ---------------------------------------------------------------------------

TEST_CASE("BLE: notify_command_result success has 3 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::Co2Calibration, true);

  REQUIRE(config_char.set_value_count == 1);
  auto entries = decode_cbor_map(config_char.last_value.data(), config_char.last_value.size());
  CHECK(entries.size() == 3);
  CHECK(find_entry(entries, "type")->text_val == "cmd_result");
  CHECK(find_entry(entries, "cmd")->text_val == "co2_cal");
  CHECK(find_entry(entries, "ok")->bool_val == true);
  CHECK(find_entry(entries, "err") == nullptr);
}

TEST_CASE("BLE: notify_command_result failure with error has 4 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::Co2Calibration, false, "sensor_not_ready");

  auto entries = decode_cbor_map(config_char.last_value.data(), config_char.last_value.size());
  CHECK(entries.size() == 4);
  CHECK(find_entry(entries, "ok")->bool_val == false);
  CHECK(find_entry(entries, "err")->text_val == "sensor_not_ready");
}

TEST_CASE("BLE: notify_command_result failure without error string has 3 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::ClearData, false);

  auto entries = decode_cbor_map(config_char.last_value.data(), config_char.last_value.size());
  CHECK(entries.size() == 3);
  CHECK(find_entry(entries, "ok")->bool_val == false);
  CHECK(find_entry(entries, "err") == nullptr);
}

// ---------------------------------------------------------------------------
// Wire format: RoutePointWire
// ---------------------------------------------------------------------------

TEST_CASE("BLE: route_point_to_wire produces 55 bytes with all valid fields") {
  RoutePoint point{};
  point.timestamp = 1711234567;
  point.gps.position.latitude = 47.376887;
  point.gps.position.longitude = 8.541694;
  point.gps.altitude_m = 408.0f;
  point.gps.fix.fix_type = GpsFixType::Fix3D;
  point.sensors.temp_hum_a.temperature = 23.5f;
  point.sensors.temp_hum_a.humidity = 45.2f;
  point.sensors.pm_a.pm_01 = 5.0f;
  point.sensors.pm_a.pm_25 = 8.3f;
  point.sensors.pm_a.pm_10 = 12.1f;
  point.sensors.co2.co2 = 450;
  point.sensors.tvoc_nox.tvoc_index = 120;
  point.sensors.tvoc_nox.nox_index = 5;
  point.sensors.pressure.pressure = 1013.2f;

  uint8_t wire[55];
  BleServiceTestAccess::route_point_to_wire(point, wire);

  // Verify timestamp at offset 0 (uint32_le)
  uint32_t ts;
  memcpy(&ts, wire, sizeof(ts));
  CHECK(ts == 1711234567);

  // Verify latitude at offset 4 (float64_le)
  double lat;
  memcpy(&lat, wire + 4, sizeof(lat));
  CHECK_THAT(lat, Catch::Matchers::WithinAbs(47.376887, 0.0001));

  // Verify longitude at offset 12 (float64_le)
  double lon;
  memcpy(&lon, wire + 12, sizeof(lon));
  CHECK_THAT(lon, Catch::Matchers::WithinAbs(8.541694, 0.0001));

  // Verify altitude at offset 20 (float32_le)
  float alt;
  memcpy(&alt, wire + 20, sizeof(alt));
  CHECK_THAT(static_cast<double>(alt), Catch::Matchers::WithinAbs(408.0, 0.1));

  // Verify gps_fix at offset 24 (uint8)
  CHECK(wire[24] == 3);

  // Verify temperature at offset 25 (float32_le)
  float temp;
  memcpy(&temp, wire + 25, sizeof(temp));
  CHECK_THAT(static_cast<double>(temp), Catch::Matchers::WithinAbs(23.5, 0.1));

  // Verify co2 at offset 45 (int16_le)
  int16_t co2;
  memcpy(&co2, wire + 45, sizeof(co2));
  CHECK(co2 == 450);

  // Verify pressure at offset 51 (float32_le)
  float pres;
  memcpy(&pres, wire + 51, sizeof(pres));
  CHECK_THAT(static_cast<double>(pres), Catch::Matchers::WithinAbs(1013.2, 0.1));
}

TEST_CASE("BLE: route_point_to_wire uses sentinels for invalid fields") {
  RoutePoint point{};
  point.timestamp = 100;
  // GPS invalid (defaults)
  point.sensors.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  point.sensors.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  point.sensors.pm_a.pm_01 = MeasuresInvalid::PM;
  point.sensors.pm_a.pm_25 = MeasuresInvalid::PM;
  point.sensors.pm_a.pm_10 = MeasuresInvalid::PM;
  point.sensors.co2.co2 = MeasuresInvalid::CO2;
  point.sensors.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  point.sensors.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  point.sensors.pressure.pressure = MeasuresInvalid::PRESSURE;

  uint8_t wire[55];
  BleServiceTestAccess::route_point_to_wire(point, wire);

  // Latitude should be GPS_LATITUDE_INVALID (91.0)
  double lat;
  memcpy(&lat, wire + 4, sizeof(lat));
  CHECK_THAT(lat, Catch::Matchers::WithinAbs(91.0, 0.001));

  // Longitude should be GPS_LONGITUDE_INVALID (181.0)
  double lon;
  memcpy(&lon, wire + 12, sizeof(lon));
  CHECK_THAT(lon, Catch::Matchers::WithinAbs(181.0, 0.001));

  // Altitude should be GPS_ALTITUDE_INVALID (-10000.0)
  float alt;
  memcpy(&alt, wire + 20, sizeof(alt));
  CHECK_THAT(static_cast<double>(alt), Catch::Matchers::WithinAbs(-10000.0, 0.1));

  // CO2 should be -1
  int16_t co2;
  memcpy(&co2, wire + 45, sizeof(co2));
  CHECK(co2 == -1);

  // TVOC should be -1
  int16_t tvoc;
  memcpy(&tvoc, wire + 47, sizeof(tvoc));
  CHECK(tvoc == -1);

  // NOx should be -1
  int16_t nox;
  memcpy(&nox, wire + 49, sizeof(nox));
  CHECK(nox == -1);
}

// ---------------------------------------------------------------------------
// String mapping
// ---------------------------------------------------------------------------

TEST_CASE("BLE: charging_state_to_str maps all values") {
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::Unknown)) ==
        "unknown");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::NotCharging)) ==
        "none");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::TrickleCharge)) ==
        "trickle");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::PreCharge)) ==
        "pre");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::FastCharge)) ==
        "fast");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::TaperCharge)) ==
        "taper");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(
            BmsChargingState::TopOffTimerActiveCharging)) == "topoff");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(
            BmsChargingState::ChargeTerminationDone)) == "done");
}

TEST_CASE("BLE: gps_mode_to_str maps all values") {
  CHECK(std::string(BleServiceTestAccess::gps_mode_to_str(GpsMode::AlwaysOff)) == "off");
  CHECK(std::string(BleServiceTestAccess::gps_mode_to_str(GpsMode::OnWhenTracking)) == "tracking");
  CHECK(std::string(BleServiceTestAccess::gps_mode_to_str(GpsMode::AlwaysOn)) == "always");
}

TEST_CASE("BLE: operating_mode_to_str maps all values") {
  CHECK(std::string(BleServiceTestAccess::operating_mode_to_str(OperatingMode::Portable)) ==
        "portable");
  CHECK(std::string(BleServiceTestAccess::operating_mode_to_str(OperatingMode::Stationary)) ==
        "stationary");
  CHECK(std::string(BleServiceTestAccess::operating_mode_to_str(OperatingMode::Offline)) ==
        "offline");
}

// ---------------------------------------------------------------------------
// Notification flow
// ---------------------------------------------------------------------------

TEST_CASE("BLE: notify_measures is no-op when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic measures_char;
  BleServiceTestAccess::set_measures_char(svc, &measures_char);
  // Not connected (default)

  svc.notify_measures(make_valid_measures(), make_valid_gps(), 100);
  CHECK(measures_char.set_value_count == 0);
  CHECK(measures_char.notify_count == 0);
}

TEST_CASE("BLE: notify_measures encodes and notifies when connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic measures_char;
  BleServiceTestAccess::set_measures_char(svc, &measures_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_measures(make_valid_measures(), make_valid_gps(), 100);
  CHECK(measures_char.set_value_count == 1);
  CHECK(measures_char.notify_count == 1);
  CHECK(!measures_char.last_value.empty());
}

TEST_CASE("BLE: update_status is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  // _status_char is nullptr by default

  // Should not crash
  svc.update_status(make_valid_power(), make_valid_gps(), false, 0);
}

TEST_CASE("BLE: update_status sets value but does not notify") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);

  svc.update_status(make_valid_power(), make_valid_gps(), true, 10042);
  CHECK(status_char.set_value_count == 1);
  CHECK(status_char.notify_count == 0);
}

TEST_CASE("BLE: update_config sets value but does not notify") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);

  svc.update_config(make_default_settings());
  CHECK(config_char.set_value_count == 1);
  CHECK(config_char.notify_count == 0);
}

TEST_CASE("BLE: notify_config is no-op when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);

  svc.notify_config(make_default_settings());
  CHECK(config_char.set_value_count == 0);
  CHECK(config_char.notify_count == 0);
}

TEST_CASE("BLE: notify_command_result is no-op when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);

  svc.notify_command_result(BleCommand::Unknown, true);
  CHECK(config_char.set_value_count == 0);
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("BLE: on_connect sets connected and stops advertising") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);

  CHECK(svc.is_connected() == false);
  BleServiceTestAccess::on_connect(svc, 1);
  CHECK(svc.is_connected() == true);
  CHECK(server.stop_advertising_count == 1);
}

TEST_CASE("BLE: on_disconnect clears state and restarts advertising") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);
  BleServiceTestAccess::set_connected(svc, true);
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 10042);

  BleServiceTestAccess::on_disconnect(svc, 1, 0);
  CHECK(svc.is_connected() == false);
  CHECK(BleServiceTestAccess::export_active(svc) == false);
  CHECK(BleServiceTestAccess::export_session_id(svc) == 0);
  CHECK(server.start_advertising_count == 1);
}

TEST_CASE("BLE: delete_all_bonds proxies to server") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);

  CHECK(svc.delete_all_bonds());
  CHECK(server.delete_all_bonds_count == 1);
}

// ---------------------------------------------------------------------------
// History download
// ---------------------------------------------------------------------------

TEST_CASE("BLE: handle_history_list is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  // _history_char is nullptr — should not crash
  svc.handle_history_list();
}

TEST_CASE("BLE: handle_history_list sends session list") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 150, 1737000000}, {10002, 300, 1737100000}};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_list();

  REQUIRE(history_char.set_value_count >= 1);
  // First byte is tag 0x00 (CBOR control)
  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  // Decode the CBOR after the tag byte
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "sessions");
}

TEST_CASE("BLE: handle_history_start sends error for non-existent session") {
  storage_spy::reset();

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_start(99999);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "error");
  CHECK(find_entry(entries, "err")->text_val == "session_not_found");
}

TEST_CASE("BLE: handle_history_start streams points and sends done") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 2, 1737000000}};

  RoutePoint pt1{};
  pt1.timestamp = 1737000000;
  pt1.sensors.temp_hum_a.temperature = 20.0f;
  pt1.sensors.temp_hum_a.humidity = 50.0f;
  RoutePoint pt2{};
  pt2.timestamp = 1737000060;
  pt2.sensors.temp_hum_a.temperature = 21.0f;
  pt2.sensors.temp_hum_a.humidity = 48.0f;
  storage_spy::points = {pt1, pt2};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_start(10001);

  // Should have sent multiple notifications:
  // 1. started (CBOR tag 0x00)
  // 2. binary data (tag 0x01)
  // 3. done (CBOR tag 0x00)
  // The last set_value is the "done" response
  REQUIRE(history_char.set_value_count >= 3);
  REQUIRE(history_char.notify_count >= 3);

  // Last notification should be "done"
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "done");
  CHECK(find_entry(entries, "sent")->uint_val == 2);
}

TEST_CASE("BLE: handle_history_fill sends error when no active download") {
  storage_spy::reset();

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  uint32_t indices[] = {0, 1};
  svc.handle_history_fill(indices, 2);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "error");
  CHECK(find_entry(entries, "err")->text_val == "no_active_download");
}

TEST_CASE("BLE: handle_history_fill retransmits requested points") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 3, 1737000000}};

  RoutePoint pt{};
  pt.timestamp = 1737000000;
  storage_spy::points = {pt, pt, pt};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 10001);

  uint32_t indices[] = {0, 2};
  svc.handle_history_fill(indices, 2);

  // Should have sent: 2 binary data + 1 done
  REQUIRE(history_char.notify_count >= 3);

  // Last notification is "done" with sent: 2
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "done");
  CHECK(find_entry(entries, "sent")->uint_val == 2);
}

TEST_CASE("BLE: handle_history_end clears export state and sends ended") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 10042);

  svc.handle_history_end();

  CHECK(BleServiceTestAccess::export_active(svc) == false);
  CHECK(BleServiceTestAccess::export_session_id(svc) == 0);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "ended");
}

TEST_CASE("BLE: deinit is no-op when not initialized") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage);
  // Should not crash
  svc.deinit();
  CHECK(svc.is_initialized() == false);
}
