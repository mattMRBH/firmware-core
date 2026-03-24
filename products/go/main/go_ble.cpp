/**
 * AirGradient Go — BLE Service implementation
 *
 * GATT setup, CBOR encoding/decoding, NimBLE callbacks, and binary
 * history streaming.  Guarded with #ifndef TEST_HOST — the BLE stack
 * is hardware-only.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_ble.h"

#ifndef TEST_HOST

#include "ag_log.h"
#include "go_storage.h"
#include "nimble_ble_server.h"
#include "rtos.h"

#include <cbor.h>
#include <esp_mac.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

static constexpr const char *TAG = "BLE";

// ---------------------------------------------------------------------------
// GATT UUIDs
// ---------------------------------------------------------------------------

static constexpr const char *SERVICE_UUID = "d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *MEASURES_CHAR_UUID = "d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *STATUS_CHAR_UUID = "d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *CONFIG_CHAR_UUID = "d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *HISTORY_CHAR_UUID = "d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Advertised name prefix. Final name is "AGo-<serial>".
static constexpr const char *ADV_NAME_PREFIX = "AGo-";
static constexpr size_t ADV_NAME_MAX_LEN = 16;

/// Minimum negotiated MTU below which notifications are suppressed.
static constexpr size_t MIN_USEFUL_MTU = 128;

/// CBOR encoding buffer size — fits all characteristic payloads.
static constexpr size_t CBOR_BUF_SIZE = 256;

/// RoutePointWire: packed binary format, 55 bytes per point.
static constexpr size_t ROUTE_POINT_WIRE_SIZE = 55;

/// History notification tag bytes.
static constexpr uint8_t HISTORY_TAG_CBOR = 0x00;
static constexpr uint8_t HISTORY_TAG_BINARY = 0x01;

/// Binary data notification header: tag (1) + point_index (2) = 3 bytes.
static constexpr size_t BINARY_HEADER_SIZE = 3;

/// Maximum ATT payload for a single notification (assumes 247-byte MTU).
/// Conservative estimate; actual limit depends on negotiated MTU.
static constexpr size_t MAX_NOTIFY_PAYLOAD = 244;

/// Maximum route points per binary notification.
static constexpr size_t POINTS_PER_NOTIFICATION =
    (MAX_NOTIFY_PAYLOAD - BINARY_HEADER_SIZE) / ROUTE_POINT_WIRE_SIZE;

/// Read batch size for route points from storage.
static constexpr uint16_t ROUTE_READ_BATCH = 4;

/// Backpressure retry delay when notify() returns false.
static constexpr uint32_t NOTIFY_RETRY_DELAY_MS = 1;

/// Maximum number of sessions in a list response.
static constexpr uint16_t MAX_SESSION_LIST = 64;

/// Firmware version string for Status characteristic.
/// TODO: Replace with build-system-provided version constant.
static constexpr const char *FW_VERSION = "0.0.0";

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BleService::BleService(RtosQueueHandle event_queue, StorageService &storage)
    : _event_queue(event_queue), _storage(storage) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool BleService::init(const char *serial) {
  if (_server != nullptr) {
    AG_LOGW(TAG, "already initialized");
    return true;
  }

  // Build advertised name: "AGo-DDEEFF"
  char adv_name[ADV_NAME_MAX_LEN] = {};
  snprintf(adv_name, sizeof(adv_name), "%s%s", ADV_NAME_PREFIX, serial ? serial : "000000");

  // Create NimBLE server (static — lives until deinit)
  static NimbleBleServer ble_server;
  _server = &ble_server;

  if (!_server->init(adv_name)) {
    AG_LOGE(TAG, "BLE stack init failed");
    _server = nullptr;
    return false;
  }

  // --- Security: Passkey Entry (Display Only) ---
  if (!_server->set_security(BleIoCapability::DISPLAY_ONLY, BleAuth::BOND | BleAuth::MITM)) {
    AG_LOGE(TAG, "set_security failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // --- GATT Service ---
  // Use auto to avoid name collision with this class (both are "BleService").
  auto *svc = _server->add_service(SERVICE_UUID);
  if (svc == nullptr) {
    AG_LOGE(TAG, "add_service failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // --- Characteristics ---

  // Measures: Notify only (requires authentication)
  _measures_char =
      svc->add_characteristic(MEASURES_CHAR_UUID, BleProperty::NOTIFY | BleProperty::READ_AUTHEN);
  if (_measures_char == nullptr) {
    AG_LOGE(TAG, "add Measures characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // Status: Read only (requires authentication)
  _status_char =
      svc->add_characteristic(STATUS_CHAR_UUID, BleProperty::READ | BleProperty::READ_AUTHEN);
  if (_status_char == nullptr) {
    AG_LOGE(TAG, "add Status characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // Config: Read + Write + Notify (requires authentication)
  _config_char = svc->add_characteristic(
      CONFIG_CHAR_UUID, BleProperty::READ | BleProperty::WRITE | BleProperty::NOTIFY |
                            BleProperty::READ_AUTHEN | BleProperty::WRITE_AUTHEN);
  if (_config_char == nullptr) {
    AG_LOGE(TAG, "add Config characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // History: Write + Notify (requires authentication)
  _history_char = svc->add_characteristic(
      HISTORY_CHAR_UUID, BleProperty::WRITE | BleProperty::NOTIFY | BleProperty::WRITE_AUTHEN);
  if (_history_char == nullptr) {
    AG_LOGE(TAG, "add History characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // --- Write callbacks ---
  _config_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { on_config_write(data, len); });
  _history_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { on_history_write(data, len); });

  // --- Start GATT service ---
  if (!svc->start()) {
    AG_LOGE(TAG, "service start failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // --- Callbacks ---
  _server->set_connect_callback([this](uint16_t conn_handle) { on_connect(conn_handle); });
  _server->set_disconnect_callback(
      [this](uint16_t conn_handle, int reason) { on_disconnect(conn_handle, reason); });
  _server->set_passkey_display_callback([this](uint32_t passkey) { on_passkey_request(passkey); });
  _server->set_auth_complete_callback([](uint16_t /*conn_handle*/, bool success) {
    AG_LOGI(TAG, "auth %s", success ? "OK" : "FAILED");
  });

  // --- Advertising ---
  if (!_server->set_advertising_name(adv_name)) {
    AG_LOGE(TAG, "set_advertising_name failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  if (!_server->add_advertised_service_uuid(SERVICE_UUID)) {
    AG_LOGE(TAG, "add_advertised_service_uuid failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  if (!_server->start_advertising()) {
    AG_LOGE(TAG, "start_advertising failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  AG_LOGI(TAG, "initialized, advertising as '%s'", adv_name);
  return true;
}

void BleService::deinit() {
  if (_server == nullptr) {
    return;
  }

  _connected.store(false);
  _export_active = false;
  _export_session_id = 0;

  _server->deinit();
  _server = nullptr;
  _measures_char = nullptr;
  _status_char = nullptr;
  _config_char = nullptr;
  _history_char = nullptr;

  AG_LOGI(TAG, "deinitialized");
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool BleService::is_initialized() const { return _server != nullptr; }

bool BleService::is_connected() const { return _connected.load(); }

// ---------------------------------------------------------------------------
// NimBLE callbacks (run in NimBLE task context)
// ---------------------------------------------------------------------------

void BleService::on_connect(uint16_t conn_handle) {
  AG_LOGI(TAG, "client connected: handle=%u", conn_handle);
  _connected.store(true);

  // Stop advertising — single connection device
  if (_server != nullptr) {
    _server->stop_advertising();
  }

  // Post BleConnected event to orchestrator
  // NOTE: Requires EventType::BleConnected in go_events.h (see header comment)
  // Event evt{};
  // evt.type = EventType::BleConnected;
  // RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_disconnect(uint16_t conn_handle, int reason) {
  AG_LOGI(TAG, "client disconnected: handle=%u reason=%d", conn_handle, reason);
  _connected.store(false);

  // Clean up active history export
  _export_active = false;
  _export_session_id = 0;

  // Restart advertising
  if (_server != nullptr) {
    _server->start_advertising();
  }

  // Post BleDisconnected event to orchestrator
  // NOTE: Requires EventType::BleDisconnected in go_events.h (see header comment)
  // Event evt{};
  // evt.type = EventType::BleDisconnected;
  // RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_config_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || len > WRITE_BUF_SIZE) {
    AG_LOGW(TAG, "config write: invalid length %u", static_cast<unsigned>(len));
    return;
  }

  _config_write_mutex.lock();
  memcpy(_config_write_buf, data, len);
  _config_write_len = len;
  _config_write_pending = true;
  _config_write_mutex.unlock();

  // Post BleConfigWrite event to orchestrator
  // NOTE: Requires EventType::BleConfigWrite in go_events.h (see header comment)
  // Event evt{};
  // evt.type = EventType::BleConfigWrite;
  // RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_history_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || len > WRITE_BUF_SIZE) {
    AG_LOGW(TAG, "history write: invalid length %u", static_cast<unsigned>(len));
    return;
  }

  _history_write_mutex.lock();
  memcpy(_history_write_buf, data, len);
  _history_write_len = len;
  _history_write_pending = true;
  _history_write_mutex.unlock();

  // Post BleHistoryWrite event to orchestrator
  // NOTE: Requires EventType::BleHistoryWrite in go_events.h (see header comment)
  // Event evt{};
  // evt.type = EventType::BleHistoryWrite;
  // RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_passkey_request(uint32_t passkey) {
  AG_LOGI(TAG, "passkey display: %06" PRIu32, passkey);

  // Post BlePairingRequest event to orchestrator
  // NOTE: Requires EventType::BlePairingRequest in go_events.h (see header comment)
  // Event evt{};
  // evt.type = EventType::BlePairingRequest;
  // evt.ble_passkey = passkey;
  // RTOS::queue_send(_event_queue, &evt);
}

// ---------------------------------------------------------------------------
// Pending write retrieval (called by orchestrator task)
// ---------------------------------------------------------------------------

size_t BleService::take_pending_config_write(uint8_t *buf, size_t buf_size) {
  _config_write_mutex.lock();
  if (!_config_write_pending) {
    _config_write_mutex.unlock();
    return 0;
  }
  size_t len = std::min(_config_write_len, buf_size);
  memcpy(buf, _config_write_buf, len);
  _config_write_pending = false;
  _config_write_len = 0;
  _config_write_mutex.unlock();
  return len;
}

size_t BleService::take_pending_history_write(uint8_t *buf, size_t buf_size) {
  _history_write_mutex.lock();
  if (!_history_write_pending) {
    _history_write_mutex.unlock();
    return 0;
  }
  size_t len = std::min(_history_write_len, buf_size);
  memcpy(buf, _history_write_buf, len);
  _history_write_pending = false;
  _history_write_len = 0;
  _history_write_mutex.unlock();
  return len;
}

// ---------------------------------------------------------------------------
// Data output: Measures
// ---------------------------------------------------------------------------

void BleService::notify_measures(const MeasuresAGo &measures, const GpsData &gps,
                                 time_t timestamp) {
  if (!_connected.load() || _measures_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_measures(buf, sizeof(buf), measures, gps, timestamp);
  if (len == 0) {
    AG_LOGW(TAG, "measures encode failed");
    return;
  }

  _measures_char->set_value(buf, len);
  _measures_char->notify();
}

// ---------------------------------------------------------------------------
// Data output: Status
// ---------------------------------------------------------------------------

void BleService::update_status(const PowerSnapshot &power, const GpsData &gps, bool tracking_active,
                               uint32_t session_id) {
  if (_status_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_status(buf, sizeof(buf), power, gps, tracking_active, session_id);
  if (len == 0) {
    AG_LOGW(TAG, "status encode failed");
    return;
  }

  _status_char->set_value(buf, len);
}

// ---------------------------------------------------------------------------
// Data output: Config
// ---------------------------------------------------------------------------

void BleService::update_config(const GoSettings &settings) {
  if (_config_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_config(buf, sizeof(buf), settings);
  if (len == 0) {
    AG_LOGW(TAG, "config encode failed");
    return;
  }

  _config_char->set_value(buf, len);
}

void BleService::notify_config(const GoSettings &settings) {
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  // Config notification includes "type": "config" discriminator plus full config
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  // 13 config keys + 1 type discriminator = 14
  cbor_encoder_create_map(&encoder, &map, 14);

  cbor_encode_text_stringz(&map, "type");
  cbor_encode_text_stringz(&map, "config");

  cbor_encode_text_stringz(&map, "meas_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.measurement_interval_seconds));

  cbor_encode_text_stringz(&map, "pm_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.pm_interval_seconds));

  cbor_encode_text_stringz(&map, "other_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.other_sensor_interval_seconds));

  cbor_encode_text_stringz(&map, "disp_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.display_refresh_interval_seconds));

  cbor_encode_text_stringz(&map, "temp_f");
  cbor_encode_boolean(&map, settings.use_fahrenheit);

  cbor_encode_text_stringz(&map, "pm_aqi");
  cbor_encode_boolean(&map, settings.pm_use_usaqi);

  cbor_encode_text_stringz(&map, "gps_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.gps_interval_seconds));

  cbor_encode_text_stringz(&map, "gps_mode");
  cbor_encode_text_stringz(&map, gps_mode_to_str(settings.gps_mode));

  cbor_encode_text_stringz(&map, "inact_to");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.inactivity_timeout_seconds));

  cbor_encode_text_stringz(&map, "auto_lock");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.auto_lock_seconds));

  cbor_encode_text_stringz(&map, "dev_name");
  cbor_encode_text_stringz(&map, settings.device_name.c_str());

  cbor_encode_text_stringz(&map, "op_mode");
  cbor_encode_text_stringz(&map, operating_mode_to_str(settings.operating_mode));

  cbor_encoder_close_container(&encoder, &map);

  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  _config_char->set_value(buf, len);
  _config_char->notify();
}

void BleService::notify_command_result(const char *cmd, bool success, const char *error) {
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  size_t map_len = 3; // type, cmd, ok
  if (!success && error != nullptr) {
    map_len = 4; // + err
  }
  cbor_encoder_create_map(&encoder, &map, map_len);

  cbor_encode_text_stringz(&map, "type");
  cbor_encode_text_stringz(&map, "cmd_result");

  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, cmd);

  cbor_encode_text_stringz(&map, "ok");
  cbor_encode_boolean(&map, success);

  if (!success && error != nullptr) {
    cbor_encode_text_stringz(&map, "err");
    cbor_encode_text_stringz(&map, error);
  }

  cbor_encoder_close_container(&encoder, &map);

  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  _config_char->set_value(buf, len);
  _config_char->notify();
}

// ---------------------------------------------------------------------------
// History: session list
// ---------------------------------------------------------------------------

void BleService::handle_history_list() {
  if (_history_char == nullptr) {
    return;
  }

  // Read session list from storage
  // NOTE: Requires StorageService::list_sessions() (see header integration notes)
  uint32_t session_ids[MAX_SESSION_LIST];
  uint16_t session_count = _storage.list_sessions(session_ids, MAX_SESSION_LIST);

  // Encode as CBOR: {"type": "sessions", "sessions": [{id, pts, ts}, ...]}
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);

  cbor_encode_text_stringz(&map, "type");
  cbor_encode_text_stringz(&map, "sessions");

  cbor_encode_text_stringz(&map, "sessions");
  CborEncoder arr;
  cbor_encoder_create_array(&map, &arr, session_count);

  for (uint16_t i = 0; i < session_count; i++) {
    CborEncoder sess_map;
    cbor_encoder_create_map(&arr, &sess_map, 3);

    cbor_encode_text_stringz(&sess_map, "id");
    cbor_encode_uint(&sess_map, session_ids[i]);

    cbor_encode_text_stringz(&sess_map, "pts");
    uint32_t pts = _storage.get_session_point_count(session_ids[i]);
    cbor_encode_uint(&sess_map, pts);

    cbor_encode_text_stringz(&sess_map, "ts");
    time_t start_ts = _storage.get_session_start_time(session_ids[i]);
    cbor_encode_uint(&sess_map, static_cast<uint64_t>(start_ts));

    cbor_encoder_close_container(&arr, &sess_map);
  }

  cbor_encoder_close_container(&map, &arr);
  cbor_encoder_close_container(&encoder, &map);

  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

// ---------------------------------------------------------------------------
// History: start download
// ---------------------------------------------------------------------------

void BleService::handle_history_start(uint32_t session_id) {
  if (_history_char == nullptr) {
    return;
  }

  // End any active download first
  if (_export_active) {
    _export_active = false;
  }

  // Check if session exists
  uint32_t total_points = _storage.get_session_point_count(session_id);
  if (total_points == 0) {
    // Send error: session not found
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, "type");
    cbor_encode_text_stringz(&map, "error");
    cbor_encode_text_stringz(&map, "err");
    cbor_encode_text_stringz(&map, "session_not_found");
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
    return;
  }

  _export_session_id = session_id;
  _export_active = true;

  // Send "started" CBOR response
  {
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 4);

    cbor_encode_text_stringz(&map, "type");
    cbor_encode_text_stringz(&map, "started");

    cbor_encode_text_stringz(&map, "session");
    cbor_encode_uint(&map, session_id);

    cbor_encode_text_stringz(&map, "total");
    cbor_encode_uint(&map, total_points);

    cbor_encode_text_stringz(&map, "pt_size");
    cbor_encode_uint(&map, ROUTE_POINT_WIRE_SIZE);

    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }

  // Stream all points as binary notifications
  uint32_t sent = 0;
  RoutePoint batch[ROUTE_READ_BATCH];

  for (uint32_t offset = 0; offset < total_points && _connected.load(); /* incremented below */) {
    uint16_t to_read = static_cast<uint16_t>(
        std::min(static_cast<uint32_t>(ROUTE_READ_BATCH), total_points - offset));
    uint16_t actually_read = _storage.read_route_points(session_id, offset, batch, to_read);

    if (actually_read == 0) {
      // Flash read error — abort stream
      AG_LOGE(TAG, "flash read error at offset %" PRIu32, offset);
      uint8_t buf[CBOR_BUF_SIZE];
      CborEncoder encoder;
      cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
      CborEncoder map;
      cbor_encoder_create_map(&encoder, &map, 2);
      cbor_encode_text_stringz(&map, "type");
      cbor_encode_text_stringz(&map, "error");
      cbor_encode_text_stringz(&map, "err");
      cbor_encode_text_stringz(&map, "flash_error");
      cbor_encoder_close_container(&encoder, &map);
      size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
      send_history_cbor(buf, len);
      _export_active = false;
      return;
    }

    // Convert to wire format and send
    uint8_t wire_buf[ROUTE_READ_BATCH * ROUTE_POINT_WIRE_SIZE];
    for (uint16_t i = 0; i < actually_read; i++) {
      route_point_to_wire(batch[i], &wire_buf[i * ROUTE_POINT_WIRE_SIZE]);
    }

    send_history_binary(static_cast<uint16_t>(offset), wire_buf,
                        actually_read * ROUTE_POINT_WIRE_SIZE);

    offset += actually_read;
    sent += actually_read;
  }

  // Send "done" CBOR response
  {
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, "type");
    cbor_encode_text_stringz(&map, "done");
    cbor_encode_text_stringz(&map, "sent");
    cbor_encode_uint(&map, sent);
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }
}

// ---------------------------------------------------------------------------
// History: fill missing points
// ---------------------------------------------------------------------------

void BleService::handle_history_fill(const uint32_t *point_indices, size_t count) {
  if (_history_char == nullptr) {
    return;
  }

  if (!_export_active) {
    // Send error: no active download
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, "type");
    cbor_encode_text_stringz(&map, "error");
    cbor_encode_text_stringz(&map, "err");
    cbor_encode_text_stringz(&map, "no_active_download");
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
    return;
  }

  uint32_t sent = 0;

  for (size_t i = 0; i < count && _connected.load(); i++) {
    RoutePoint point;
    uint16_t actually_read =
        _storage.read_route_points(_export_session_id, point_indices[i], &point, 1);

    if (actually_read == 0) {
      AG_LOGW(TAG, "fill: could not read point %" PRIu32, point_indices[i]);
      continue;
    }

    uint8_t wire_buf[ROUTE_POINT_WIRE_SIZE];
    route_point_to_wire(point, wire_buf);
    send_history_binary(static_cast<uint16_t>(point_indices[i]), wire_buf, ROUTE_POINT_WIRE_SIZE);
    sent++;
  }

  // Send "done" CBOR response
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, "type");
  cbor_encode_text_stringz(&map, "done");
  cbor_encode_text_stringz(&map, "sent");
  cbor_encode_uint(&map, sent);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

// ---------------------------------------------------------------------------
// History: end download
// ---------------------------------------------------------------------------

void BleService::handle_history_end() {
  _export_active = false;
  _export_session_id = 0;

  if (_history_char == nullptr) {
    return;
  }

  // Send "ended" CBOR response
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 1);
  cbor_encode_text_stringz(&map, "type");
  cbor_encode_text_stringz(&map, "ended");
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

// ---------------------------------------------------------------------------
// History: notification helpers
// ---------------------------------------------------------------------------

bool BleService::send_history_cbor(const uint8_t *cbor_data, size_t cbor_len) {
  if (_history_char == nullptr || cbor_len == 0) {
    return false;
  }

  // Prepend tag byte 0x00
  uint8_t tagged[CBOR_BUF_SIZE + 1];
  tagged[0] = HISTORY_TAG_CBOR;
  size_t total = 1 + cbor_len;
  if (total > sizeof(tagged)) {
    AG_LOGW(TAG, "history CBOR too large: %u bytes", static_cast<unsigned>(total));
    return false;
  }
  memcpy(&tagged[1], cbor_data, cbor_len);

  _history_char->set_value(tagged, total);

  // Retry with backpressure
  while (!_history_char->notify()) {
    if (!_connected.load()) {
      return false;
    }
    RTOS::delay_ms(NOTIFY_RETRY_DELAY_MS);
  }
  return true;
}

bool BleService::send_history_binary(uint16_t first_point_index, const uint8_t *data,
                                     size_t data_len) {
  if (_history_char == nullptr || data_len == 0) {
    return false;
  }

  // Build notification: [tag] [uint16_le index] [data...]
  uint8_t packet[MAX_NOTIFY_PAYLOAD];
  size_t total = BINARY_HEADER_SIZE + data_len;
  if (total > sizeof(packet)) {
    AG_LOGW(TAG, "history binary too large: %u bytes", static_cast<unsigned>(total));
    return false;
  }

  packet[0] = HISTORY_TAG_BINARY;
  // Little-endian uint16
  packet[1] = static_cast<uint8_t>(first_point_index & 0xFF);
  packet[2] = static_cast<uint8_t>((first_point_index >> 8) & 0xFF);
  memcpy(&packet[BINARY_HEADER_SIZE], data, data_len);

  _history_char->set_value(packet, total);

  // Retry with backpressure
  while (!_history_char->notify()) {
    if (!_connected.load()) {
      return false;
    }
    RTOS::delay_ms(NOTIFY_RETRY_DELAY_MS);
  }
  return true;
}

// ---------------------------------------------------------------------------
// RoutePointWire conversion
// ---------------------------------------------------------------------------

void BleService::route_point_to_wire(const RoutePoint &point, uint8_t *out) {
  // 55 bytes, packed little-endian.
  // Use memcpy for type-punning safety (platform is little-endian on ESP32).

  size_t offset = 0;

  auto write_u32 = [&](uint32_t v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_f64 = [&](double v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_f32 = [&](float v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_u8 = [&](uint8_t v) {
    out[offset] = v;
    offset += 1;
  };
  auto write_i16 = [&](int16_t v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };

  // timestamp (4)
  write_u32(static_cast<uint32_t>(point.timestamp));

  // GPS position
  if (is_latitude_valid(point.gps.position.latitude)) {
    write_f64(point.gps.position.latitude);
  } else {
    write_f64(GPS_LATITUDE_INVALID);
  }

  if (is_longitude_valid(point.gps.position.longitude)) {
    write_f64(point.gps.position.longitude);
  } else {
    write_f64(GPS_LONGITUDE_INVALID);
  }

  // altitude (4)
  if (is_altitude_valid(point.gps.altitude_m)) {
    write_f32(point.gps.altitude_m);
  } else {
    write_f32(GPS_ALTITUDE_INVALID);
  }

  // gps_fix (1)
  write_u8(static_cast<uint8_t>(point.gps.fix.fix_type));

  // Sensor data — use sentinel values for invalid fields
  const auto &th = point.sensors.temp_hum_a;
  write_f32(th.is_temp_valid() ? th.temperature : MeasuresInvalid::TEMPERATURE);
  write_f32(th.is_hum_valid() ? th.humidity : MeasuresInvalid::HUMIDITY);

  const auto &pm = point.sensors.pm_a;
  write_f32(pm.is_pm_01_valid() ? pm.pm_01 : MeasuresInvalid::PM);
  write_f32(pm.is_pm_25_valid() ? pm.pm_25 : MeasuresInvalid::PM);
  write_f32(pm.is_pm_10_valid() ? pm.pm_10 : MeasuresInvalid::PM);

  const auto &co2 = point.sensors.co2;
  write_i16(co2.is_valid() ? static_cast<int16_t>(co2.co2) : static_cast<int16_t>(-1));

  const auto &tvoc_nox = point.sensors.tvoc_nox;
  write_i16(tvoc_nox.is_tvoc_index_valid() ? static_cast<int16_t>(tvoc_nox.tvoc_index)
                                           : static_cast<int16_t>(-1));
  write_i16(tvoc_nox.is_nox_index_valid() ? static_cast<int16_t>(tvoc_nox.nox_index)
                                          : static_cast<int16_t>(-1));

  // pressure (4)
  const auto &pres = point.sensors.pressure;
  write_f32(pres.is_pressure_valid() ? pres.pressure : MeasuresInvalid::PRESSURE);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Measures
// ---------------------------------------------------------------------------

size_t BleService::encode_measures(uint8_t *buf, size_t buf_size, const MeasuresAGo &m,
                                   const GpsData &gps, time_t ts) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // Count fields to determine map size (ts is always present)
  size_t field_count = 1; // "ts"

  const auto &th = m.temp_hum_a;
  if (th.is_temp_valid())
    field_count++;
  if (th.is_hum_valid())
    field_count++;

  const auto &pm = m.pm_a;
  if (pm.is_pm_01_valid())
    field_count++;
  if (pm.is_pm_25_valid())
    field_count++;
  if (pm.is_pm_10_valid())
    field_count++;

  if (m.co2.is_valid())
    field_count++;
  if (m.tvoc_nox.is_tvoc_index_valid())
    field_count++;
  if (m.tvoc_nox.is_nox_index_valid())
    field_count++;

  if (m.pressure.is_pressure_valid())
    field_count++;

  // GPS fields are only included when tracking (indicated by fix type check).
  // The caller (orchestrator) only calls notify_measures() when in Tracking
  // behavior, but we check fix availability as a defensive guard.
  // GPS fields: lat, lon, alt, fix, sat — always included as a group when tracking.
  bool include_gps = is_fix_valid(gps.fix);
  if (include_gps) {
    // lat, lon conditionally based on validity
    if (is_latitude_valid(gps.position.latitude))
      field_count++;
    if (is_longitude_valid(gps.position.longitude))
      field_count++;
    if (is_altitude_valid(gps.altitude_m))
      field_count++;
    field_count++; // "fix" (always when GPS included)
    field_count++; // "sat" (always when GPS included)
  }

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, field_count);

  // Sensor fields — omit invalid
  if (th.is_temp_valid()) {
    cbor_encode_text_stringz(&map, "t");
    cbor_encode_float(&map, th.temperature);
  }
  if (th.is_hum_valid()) {
    cbor_encode_text_stringz(&map, "h");
    cbor_encode_float(&map, th.humidity);
  }

  if (pm.is_pm_01_valid()) {
    cbor_encode_text_stringz(&map, "pm1");
    cbor_encode_float(&map, pm.pm_01);
  }
  if (pm.is_pm_25_valid()) {
    cbor_encode_text_stringz(&map, "pm25");
    cbor_encode_float(&map, pm.pm_25);
  }
  if (pm.is_pm_10_valid()) {
    cbor_encode_text_stringz(&map, "pm10");
    cbor_encode_float(&map, pm.pm_10);
  }

  if (m.co2.is_valid()) {
    cbor_encode_text_stringz(&map, "co2");
    cbor_encode_uint(&map, static_cast<uint64_t>(m.co2.co2));
  }

  if (m.tvoc_nox.is_tvoc_index_valid()) {
    cbor_encode_text_stringz(&map, "tvoc");
    cbor_encode_uint(&map, static_cast<uint64_t>(m.tvoc_nox.tvoc_index));
  }
  if (m.tvoc_nox.is_nox_index_valid()) {
    cbor_encode_text_stringz(&map, "nox");
    cbor_encode_uint(&map, static_cast<uint64_t>(m.tvoc_nox.nox_index));
  }

  if (m.pressure.is_pressure_valid()) {
    cbor_encode_text_stringz(&map, "pres");
    cbor_encode_float(&map, m.pressure.pressure);
  }

  // GPS fields (only when tracking / fix valid)
  if (include_gps) {
    if (is_latitude_valid(gps.position.latitude)) {
      cbor_encode_text_stringz(&map, "lat");
      cbor_encode_double(&map, gps.position.latitude);
    }
    if (is_longitude_valid(gps.position.longitude)) {
      cbor_encode_text_stringz(&map, "lon");
      cbor_encode_double(&map, gps.position.longitude);
    }
    if (is_altitude_valid(gps.altitude_m)) {
      cbor_encode_text_stringz(&map, "alt");
      cbor_encode_float(&map, gps.altitude_m);
    }
    cbor_encode_text_stringz(&map, "fix");
    cbor_encode_uint(&map, static_cast<uint64_t>(gps.fix.fix_type));

    cbor_encode_text_stringz(&map, "sat");
    cbor_encode_uint(&map, static_cast<uint64_t>(is_satellite_count_valid(gps.fix.satellite_count)
                                                     ? gps.fix.satellite_count
                                                     : 0));
  }

  // Timestamp is always present
  cbor_encode_text_stringz(&map, "ts");
  cbor_encode_uint(&map, static_cast<uint64_t>(ts));

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Status
// ---------------------------------------------------------------------------

size_t BleService::encode_status(uint8_t *buf, size_t buf_size, const PowerSnapshot &power,
                                 const GpsData &gps, bool tracking, uint32_t session_id) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // All 10 keys are always present
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 10);

  cbor_encode_text_stringz(&map, "gps_fix");
  cbor_encode_uint(&map, static_cast<uint64_t>(gps.fix.fix_type));

  cbor_encode_text_stringz(&map, "gps_sat");
  cbor_encode_uint(&map, static_cast<uint64_t>(is_satellite_count_valid(gps.fix.satellite_count)
                                                   ? gps.fix.satellite_count
                                                   : 0));

  cbor_encode_text_stringz(&map, "bat_pct");
  cbor_encode_uint(&map, static_cast<uint64_t>(power.battery_percentage >= 0.0f
                                                   ? static_cast<uint32_t>(power.battery_percentage)
                                                   : 0));

  cbor_encode_text_stringz(&map, "bat_v");
  cbor_encode_float(&map, power.battery_voltage >= 0.0f ? power.battery_voltage : 0.0f);

  cbor_encode_text_stringz(&map, "charging");
  cbor_encode_text_stringz(&map, charging_state_to_str(power.charging_status));

  cbor_encode_text_stringz(&map, "tracking");
  cbor_encode_boolean(&map, tracking);

  cbor_encode_text_stringz(&map, "session");
  cbor_encode_uint(&map, session_id);

  // Flash usage — requires NandStorage extensions (see integration notes)
  // TODO: Replace with actual NandStorage::total_capacity_kb() and used_kb()
  cbor_encode_text_stringz(&map, "flash_kb");
  cbor_encode_uint(&map, 0);

  cbor_encode_text_stringz(&map, "used_kb");
  cbor_encode_uint(&map, 0);

  cbor_encode_text_stringz(&map, "fw");
  cbor_encode_text_stringz(&map, FW_VERSION);

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Config
// ---------------------------------------------------------------------------

size_t BleService::encode_config(uint8_t *buf, size_t buf_size, const GoSettings &settings) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // 12 config keys
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 12);

  cbor_encode_text_stringz(&map, "meas_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.measurement_interval_seconds));

  cbor_encode_text_stringz(&map, "pm_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.pm_interval_seconds));

  cbor_encode_text_stringz(&map, "other_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.other_sensor_interval_seconds));

  cbor_encode_text_stringz(&map, "disp_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.display_refresh_interval_seconds));

  cbor_encode_text_stringz(&map, "temp_f");
  cbor_encode_boolean(&map, settings.use_fahrenheit);

  cbor_encode_text_stringz(&map, "pm_aqi");
  cbor_encode_boolean(&map, settings.pm_use_usaqi);

  cbor_encode_text_stringz(&map, "gps_int");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.gps_interval_seconds));

  cbor_encode_text_stringz(&map, "gps_mode");
  cbor_encode_text_stringz(&map, gps_mode_to_str(settings.gps_mode));

  cbor_encode_text_stringz(&map, "inact_to");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.inactivity_timeout_seconds));

  cbor_encode_text_stringz(&map, "auto_lock");
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.auto_lock_seconds));

  cbor_encode_text_stringz(&map, "dev_name");
  cbor_encode_text_stringz(&map, settings.device_name.c_str());

  cbor_encode_text_stringz(&map, "op_mode");
  cbor_encode_text_stringz(&map, operating_mode_to_str(settings.operating_mode));

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// String mapping helpers
// ---------------------------------------------------------------------------

const char *BleService::charging_state_to_str(BmsChargingState state) {
  switch (state) {
  case BmsChargingState::NotCharging:
    return "none";
  case BmsChargingState::TrickleCharge:
    return "trickle";
  case BmsChargingState::PreCharge:
    return "pre";
  case BmsChargingState::FastCharge:
    return "fast";
  case BmsChargingState::TaperCharge:
    return "taper";
  case BmsChargingState::TopOffTimerActiveCharging:
    return "topoff";
  case BmsChargingState::ChargeTerminationDone:
    return "done";
  case BmsChargingState::Unknown:
  default:
    return "unknown";
  }
}

const char *BleService::gps_mode_to_str(GpsMode mode) {
  switch (mode) {
  case GpsMode::AlwaysOff:
    return "off";
  case GpsMode::OnWhenTracking:
    return "tracking";
  case GpsMode::AlwaysOn:
    return "always";
  default:
    return "tracking";
  }
}

const char *BleService::operating_mode_to_str(OperatingMode mode) {
  switch (mode) {
  case OperatingMode::Portable:
    return "portable";
  case OperatingMode::Stationary:
    return "stationary";
  case OperatingMode::Offline:
    return "offline";
  default:
    return "offline";
  }
}

#endif // TEST_HOST

// --- Integration Notes ---
//
// go_events.h:
//   Add EventType entries: BleConnected, BleDisconnected, BleConfigWrite,
//   BleHistoryWrite, BlePairingRequest.
//   Add Event union member: uint32_t ble_passkey;
//
// go_orchestrator.h/.cpp:
//   Add BleService member to Orchestrator::Services.
//   Add event dispatch handlers for all BLE event types.
//   Wire init()/deinit() into mode transitions (Portable enter/leave).
//   Forward sensor data via notify_measures() on SensorDataReady when BLE connected.
//
// go_storage.h/.cpp:
//   Add read methods: list_sessions(), get_session_point_count(),
//   read_route_points(), get_session_start_time().
//
// go_display.cpp / go_ui.cpp:
//   Add passkey display overlay for BlePairingRequest events.
//   BLE connected icon is already scaffolded.
//
// nand_storage.h:
//   Add total_capacity_kb() and used_kb() methods for Status characteristic
//   flash usage reporting.
//
// sdkconfig:
//   Full sdkconfig may need regeneration after adding NimBLE defaults.
//
// esp-nimble-cpp submodule:
//   Must be initialized: git submodule update --init
