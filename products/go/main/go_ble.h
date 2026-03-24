/**
 * AirGradient Go — BLE Service
 *
 * BLE peripheral service exposing sensor measurements, device status,
 * configuration, and stored route data to a connected phone app over a
 * single custom GATT service.  Active only in Portable operating mode.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

// ---------------------------------------------------------------------------
// Event types required in go_events.h for compilation:
//
//   EventType::BleConnected        — no payload
//   EventType::BleDisconnected     — no payload
//   EventType::BleConfigWrite      — no payload (data in BleService pending buffer)
//   EventType::BleHistoryWrite     — no payload (data in BleService pending buffer)
//   EventType::BlePairingRequest   — payload: uint32_t ble_passkey
//
// Add to the Event union:
//   uint32_t ble_passkey;  // BlePairingRequest
// ---------------------------------------------------------------------------

#include "go_power.h"
#include "go_settings.h"
#include "go_types.h"
#include "hal/ble_server.h"
#include "measures_types.h"
#include "rtos.h"
#include "types/gps_types.h"

#include <atomic>
#include <cstdint>
#include <ctime>

class StorageService; // forward declaration

class BleService {
public:
  // --- Construction ---

  /// event_queue: shared orchestrator event queue (for posting BLE events)
  /// storage: storage service reference (for history export reads)
  explicit BleService(RtosQueueHandle event_queue, StorageService &storage);

  // --- Lifecycle (called by orchestrator) ---

  /// Initialize BLE stack, register GATT service and characteristics,
  /// configure security (passkey display), and start advertising.
  /// serial: device serial string for the advertised name (e.g., "DDEEFF").
  /// Returns false if BLE stack init fails.
  bool init(const char *serial);

  /// Stop advertising, disconnect clients, tear down BLE stack.
  /// Safe to call when not initialized (no-op).
  void deinit();

  // --- Data output (called by orchestrator in orchestrator task context) ---

  /// Encode measures + GPS as CBOR and send notification.
  /// No-op if no client is subscribed to Measures.
  void notify_measures(const MeasuresAGo &measures, const GpsData &gps, time_t timestamp);

  /// Update the Status characteristic value (CBOR-encoded).
  /// Called after BMS poll, GPS fix change, or tracking state change.
  void update_status(const PowerSnapshot &power, const GpsData &gps, bool tracking_active,
                     uint32_t session_id);

  /// Update the readable Config characteristic value with current settings.
  /// Called after settings change from any source.
  void update_config(const GoSettings &settings);

  /// Send a Config notification with the full current config.
  /// Called after a config change is applied (from BLE write or display UI).
  void notify_config(const GoSettings &settings);

  /// Send a command result notification on the Config characteristic.
  void notify_command_result(const char *cmd, bool success, const char *error = nullptr);

  // --- Pending write data (called by orchestrator after BLE events) ---

  /// Retrieve the raw CBOR bytes from the last Config write.
  /// Returns the number of bytes copied to buf. Returns 0 if no pending data.
  /// Clears the pending flag after retrieval.
  size_t take_pending_config_write(uint8_t *buf, size_t buf_size);

  /// Retrieve the raw CBOR bytes from the last History write.
  /// Returns the number of bytes copied to buf. Returns 0 if no pending data.
  /// Clears the pending flag after retrieval.
  size_t take_pending_history_write(uint8_t *buf, size_t buf_size);

  // --- History download (called by orchestrator after decoding history command) ---

  /// Send session list as CBOR notification.
  void handle_history_list();

  /// Stream all points for a session as binary notifications. Blocks until
  /// complete. Sends CBOR "started" before and "done" after the stream.
  void handle_history_start(uint32_t session_id);

  /// Retransmit specific points by index. Sends binary data notifications
  /// for each requested point, followed by a CBOR "done".
  void handle_history_fill(const uint32_t *point_indices, size_t count);

  /// Close the current download session.
  void handle_history_end();

  // --- State queries ---

  bool is_initialized() const;
  bool is_connected() const;

#ifdef TEST_HOST
  friend class BleServiceTestAccess;
#endif

private:
  RtosQueueHandle _event_queue;
  StorageService &_storage;

  BleServer *_server = nullptr;
  BleCharacteristic *_measures_char = nullptr;
  BleCharacteristic *_status_char = nullptr;
  BleCharacteristic *_config_char = nullptr;
  BleCharacteristic *_history_char = nullptr;

  std::atomic<bool> _connected{false};

  // --- Pending write buffers (written by NimBLE callbacks, read by orchestrator) ---

  static constexpr size_t WRITE_BUF_SIZE = 256;

  RtosMutex _config_write_mutex;
  uint8_t _config_write_buf[WRITE_BUF_SIZE] = {};
  size_t _config_write_len = 0;
  bool _config_write_pending = false;

  RtosMutex _history_write_mutex;
  uint8_t _history_write_buf[WRITE_BUF_SIZE] = {};
  size_t _history_write_len = 0;
  bool _history_write_pending = false;

  // --- History export state ---

  uint32_t _export_session_id = 0;
  bool _export_active = false;

  // --- Internal helpers ---

  void on_connect(uint16_t conn_handle);
  void on_disconnect(uint16_t conn_handle, int reason);
  void on_config_write(const uint8_t *data, size_t len);
  void on_history_write(const uint8_t *data, size_t len);
  void on_passkey_request(uint32_t passkey);

  /// CBOR encoding helpers (stack-allocated buffers, no heap)
  size_t encode_measures(uint8_t *buf, size_t buf_size, const MeasuresAGo &m, const GpsData &gps,
                         time_t ts);
  size_t encode_status(uint8_t *buf, size_t buf_size, const PowerSnapshot &power,
                       const GpsData &gps, bool tracking, uint32_t session_id);
  size_t encode_config(uint8_t *buf, size_t buf_size, const GoSettings &settings);

  /// Send a CBOR control response on the History characteristic (tag 0x00).
  bool send_history_cbor(const uint8_t *cbor_data, size_t cbor_len);

  /// Send a binary data chunk on the History characteristic (tag 0x01).
  /// Retries with backpressure if the TX buffer is full.
  bool send_history_binary(uint16_t first_point_index, const uint8_t *data, size_t data_len);

  /// Convert a RoutePoint to the 55-byte RoutePointWire binary format.
  static void route_point_to_wire(const struct RoutePoint &point, uint8_t *out);

  /// Map BmsChargingState to CBOR text value.
  static const char *charging_state_to_str(BmsChargingState state);

  /// Map GpsMode to CBOR text value.
  static const char *gps_mode_to_str(GpsMode mode);

  /// Map OperatingMode to CBOR text value.
  static const char *operating_mode_to_str(OperatingMode mode);
};

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
