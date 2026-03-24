# BLE Service

BLE peripheral service for AirGradient Go. Exposes sensor measurements,
device status, configuration, and stored route data to a connected phone app
over a single custom GATT service. Active only in Portable operating mode.
Called synchronously by the orchestrator for data output; NimBLE callbacks run
in the NimBLE task and post lightweight events to the orchestrator queue.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_ble.h` | `BleService` class declaration |
| `products/go/main/go_ble.cpp` | GATT setup, CBOR encoding, NimBLE callbacks, binary history streaming |
| `products/go/specs/ble_service.md` | Feature spec (design rationale and protocol decisions) |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `NimbleBleServer` | `airgradient-ble` (`drivers/nimble_ble_server.h`) | Concrete NimBLE-backed BLE server (static instance in `init()`) |
| `AgBleServer`, `AgBleGattService`, `AgBleCharacteristic` | `airgradient-ble` (`hal/ble_server.h`) | Abstract BLE HAL interfaces |
| `AgBleProperty`, `AgBleIoCapability`, `AgBleAuth` | `airgradient-ble` (`hal/ble_types.h`) | Property flags, security enums, callback typedefs |
| `espressif/cbor` | ESP-IDF managed dependency (`^0.6.0~1`) | TinyCBOR `CborEncoder` for all CBOR payloads |
| `MeasuresAGo` | `airgradient-common` (`measures_types.h`) | Sensor measurement data + field-level `is_*_valid()` methods |
| `GpsData` | `airgradient-gps` (`types/gps_types.h`) | GPS position/fix data + `is_fix_valid()`, `is_latitude_valid()`, etc. |
| `PowerSnapshot` | product (`go_power.h`) | Battery voltage, percentage, charging state |
| `GoSettings` | product (`go_settings.h`) | Device configuration struct (12 fields) |
| `StorageService` | product (`go_storage.h`) | Route data read for history export (requires extensions — see §Known Compilation Blockers) |
| `RTOS`, `RtosMutex` | `airgradient-common` (`rtos.h`) | `delay_ms()`, `queue_send()`, mutex for pending write buffers |

---

## GATT Profile

### Service

| Field | Value |
|---|---|
| UUID | `d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1` |
| Type | Primary Service |

### Characteristics

| Name | UUID | Properties | Auth | Description |
|---|---|---|---|---|
| Measures | `d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1` | Notify | — | Live sensor + GPS stream (CBOR) |
| Status | `d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1` | Read | `READ_AUTHEN` | Device status snapshot (CBOR) |
| Config | `d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1` | Read, Write, Notify | `READ_AUTHEN`, `WRITE_AUTHEN` | Get/set config, execute commands (CBOR) |
| History | `d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1` | Write, Notify | `WRITE_AUTHEN` | Stored route data export (CBOR control + binary data) |

All characteristics require an authenticated (encrypted) link — unencrypted
reads/writes are rejected by the NimBLE stack.

---

## Advertising

The device advertises as `AGo-<serial>`. The serial is derived from the last
3 bytes of the Wi-Fi station MAC formatted as uppercase hex.

Example: MAC `AA:BB:CC:DD:EE:FF` -> name `AGo-DDEEFF`.

Implementation detail: `init()` receives the serial as a parameter. The caller
(orchestrator / `main.cpp`) reads the MAC via `esp_read_mac()` and formats it.
The BLE service itself does not read the MAC.

The 128-bit service UUID is placed in the advertising payload. The complete
local name goes in the scan response (the UUID plus AD flags consume 21 of the
31-byte advertising payload, leaving insufficient room for the full name).

Advertising is single-connection: `on_connect()` calls `stop_advertising()`,
`on_disconnect()` calls `start_advertising()`.

---

## Security

### Pairing Model

Passkey Entry with Display Only IO capability. The BLE SMP specification
mandates a 6-digit numeric passkey (000000-999999).

Implementation (`go_ble.cpp:119`):

```cpp
_server->set_security(AgBleIoCapability::DISPLAY_ONLY, AgBleAuth::BOND | AgBleAuth::MITM);
```

### Pairing Flow

1. Device advertises, phone discovers and connects.
2. Phone initiates pairing.
3. NimBLE generates a random 6-digit passkey.
4. NimBLE invokes the passkey display callback -> `on_passkey_request()`.
5. `on_passkey_request()` logs the passkey and posts a `BlePairingRequest`
   event (carrying the passkey) to the orchestrator queue.
6. Orchestrator renders the passkey on the e-paper display (pairing overlay).
7. User enters the passkey on the phone.
8. NimBLE completes the pairing handshake (`set_auth_complete_callback` logs
   success/failure).
9. On success, NimBLE stores the bond in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST`).

### Bonding

Bonded devices reconnect automatically without re-entering the passkey.
Bond data is persisted in NVS across power cycles.

### Implementation Note

Event posting in `on_passkey_request()` is currently commented out because
`EventType::BlePairingRequest` does not exist in `go_events.h` yet. The passkey
is logged via `AG_LOGI` as a temporary measure.

---

## Serialization

All characteristic payloads use CBOR (RFC 8949) encoded with TinyCBOR's
`CborEncoder`. All encoding uses a stack-allocated 256-byte buffer
(`CBOR_BUF_SIZE`). No heap allocation during encoding.

### Conventions

- **Map keys**: Short lowercase strings (`"t"`, `"pm25"`, `"co2"`)
- **Invalid fields**: Omit the key entirely (missing key = sensor not available)
- **Numeric types**: `cbor_encode_uint` for integers (CO2, TVOC, NOx),
  `cbor_encode_float` for float32 (temp, humidity, PM, pressure, altitude),
  `cbor_encode_double` for float64 (GPS lat/lon)

### Estimated Payload Sizes

| Characteristic | Typical size | Max size | Fits in 253B ATT? |
|---|---|---|---|
| Measures | ~120B | ~135B | Yes |
| Status | ~110B | ~130B | Yes |
| Config (read, 12 keys) | ~140B | ~170B | Yes |
| Config (notify, 13 keys + type) | ~155B | ~185B | Yes |
| History control (CBOR) | ~40B | ~180B | Yes |
| History data (binary, 4 pts) | 223B | 223B | Yes |

---

## Characteristic: Measures

### Trigger

The orchestrator calls `notify_measures()` when all of the following are true:
- Operating mode is Portable
- A BLE client is connected (`_connected` is true)
- A `SensorDataReady` event was received

### CBOR Payload (Map)

| Key | CBOR Type | Source | Unit | Omit when |
|---|---|---|---|---|
| `"t"` | float32 | `TempHumData::temperature` | C | `is_temp_valid()` false |
| `"h"` | float32 | `TempHumData::humidity` | % | `is_hum_valid()` false |
| `"pm1"` | float32 | `PMData::pm_01` | ug/m3 | `is_pm_01_valid()` false |
| `"pm25"` | float32 | `PMData::pm_25` | ug/m3 | `is_pm_25_valid()` false |
| `"pm10"` | float32 | `PMData::pm_10` | ug/m3 | `is_pm_10_valid()` false |
| `"co2"` | uint | `CO2Data::co2` | ppm | `is_valid()` false |
| `"tvoc"` | uint | `TVOCNOxData::tvoc_index` | index | `is_tvoc_index_valid()` false |
| `"nox"` | uint | `TVOCNOxData::nox_index` | index | `is_nox_index_valid()` false |
| `"pres"` | float32 | `PressureData::pressure` | hPa | `is_pressure_valid()` false |
| `"lat"` | float64 | `GpsPosition::latitude` | degrees | `is_latitude_valid()` false |
| `"lon"` | float64 | `GpsPosition::longitude` | degrees | `is_longitude_valid()` false |
| `"alt"` | float32 | `GpsData::altitude_m` | meters MSL | `is_altitude_valid()` false |
| `"fix"` | uint | `GpsFixType` | 0=none, 2=2D, 3=3D | GPS not included |
| `"sat"` | uint | `GpsFix::satellite_count` | count | GPS not included |
| `"ts"` | uint | system time (`time_t`) | unix seconds | **Always present** |

**GPS field inclusion rule** (implemented in `encode_measures()`): GPS fields
(`"lat"`, `"lon"`, `"alt"`, `"fix"`, `"sat"`) are only included when
`is_fix_valid(gps.fix)` returns true. When the device is Idle (not tracking),
GPS may be off entirely, so these keys are omitted. The `"fix"` and `"sat"`
keys are always included as a group when GPS is included; `"lat"`, `"lon"`,
`"alt"` are individually omitted if their specific validity check fails.

### Examples

**Tracking, all sensors ready, GPS has fix:**

```cbor
{"t": 23.5, "h": 45.2, "pm1": 5.0, "pm25": 8.3, "pm10": 12.1,
 "co2": 450, "tvoc": 120, "nox": 5, "pres": 1013.2,
 "lat": 47.376887, "lon": 8.541694, "alt": 408.0,
 "fix": 3, "sat": 12, "ts": 1711234567}
```

**Idle (not tracking), CO2 warming up:**

```cbor
{"t": 23.5, "h": 45.2, "pm1": 5.0, "pm25": 8.3, "pm10": 12.1,
 "tvoc": 120, "nox": 5, "pres": 1013.2,
 "ts": 1711234567}
```

GPS keys absent (not tracking). CO2 key absent (`is_valid()` returned false).

---

## Characteristic: Status

### Trigger

Read-only. The BLE service updates the characteristic value when the
orchestrator calls `update_status()`. This happens after each BMS poll,
GPS fix change, or tracking state change.

### CBOR Payload (Map) — All 10 Keys Always Present

| Key | CBOR Type | Source | Description |
|---|---|---|---|
| `"gps_fix"` | uint | `GpsFixType` | 0=none, 2=2D, 3=3D |
| `"gps_sat"` | uint | `satellite_count` | 0 if `is_satellite_count_valid()` false |
| `"bat_pct"` | uint | `PowerSnapshot::battery_percentage` | 0-100 (%), 0 if negative |
| `"bat_v"` | float32 | `PowerSnapshot::battery_voltage` | Volts, 0.0 if negative |
| `"charging"` | text | `BmsChargingState` | See mapping table below |
| `"tracking"` | bool | `tracking_active` parameter | Currently tracking? |
| `"session"` | uint | `session_id` parameter | 0 if not tracking |
| `"flash_kb"` | uint | NandStorage | **Stubbed to 0** (TODO: requires `NandStorage` extensions) |
| `"used_kb"` | uint | NandStorage | **Stubbed to 0** (TODO: requires `NandStorage` extensions) |
| `"fw"` | text | `FW_VERSION` constant | Currently `"0.0.0"` (TODO: build-system version) |

### Charging State Mapping

Implemented in `charging_state_to_str()`:

| `BmsChargingState` | CBOR text |
|---|---|
| `NotCharging` | `"none"` |
| `TrickleCharge` | `"trickle"` |
| `PreCharge` | `"pre"` |
| `FastCharge` | `"fast"` |
| `TaperCharge` | `"taper"` |
| `TopOffTimerActiveCharging` | `"topoff"` |
| `ChargeTerminationDone` | `"done"` |
| `Unknown` (and default) | `"unknown"` |

---

## Characteristic: Config

Supports three operations through a single characteristic: **read current
config**, **set config values**, and **execute commands**.

### Read (phone reads characteristic)

Returns the full device configuration as a 12-key CBOR map. The BLE service
keeps this value updated whenever the orchestrator calls `update_config()`.

#### CBOR Payload (Map) — 12 Keys

| Key | CBOR Type | `GoSettings` field | Encoded with |
|---|---|---|---|
| `"meas_int"` | uint | `measurement_interval_seconds` | `cbor_encode_uint` |
| `"pm_int"` | uint | `pm_interval_seconds` | `cbor_encode_uint` |
| `"other_int"` | uint | `other_sensor_interval_seconds` | `cbor_encode_uint` |
| `"disp_int"` | uint | `display_refresh_interval_seconds` | `cbor_encode_uint` |
| `"temp_f"` | bool | `use_fahrenheit` | `cbor_encode_boolean` |
| `"pm_aqi"` | bool | `pm_use_usaqi` | `cbor_encode_boolean` |
| `"gps_int"` | uint | `gps_interval_seconds` | `cbor_encode_uint` |
| `"gps_mode"` | text | `gps_mode` | See mapping below |
| `"inact_to"` | uint | `inactivity_timeout_seconds` | `cbor_encode_uint` |
| `"auto_lock"` | uint | `auto_lock_seconds` | `cbor_encode_uint` |
| `"dev_name"` | text | `device_name` | `cbor_encode_text_stringz` |
| `"op_mode"` | text | `operating_mode` | See mapping below |

#### GpsMode Mapping (`gps_mode_to_str()`)

| `GpsMode` | CBOR text |
|---|---|
| `AlwaysOff` | `"off"` |
| `OnWhenTracking` | `"tracking"` |
| `AlwaysOn` | `"always"` |

#### OperatingMode Mapping (`operating_mode_to_str()`)

| `OperatingMode` | CBOR text |
|---|---|
| `Portable` | `"portable"` |
| `Stationary` | `"stationary"` |
| `Offline` | `"offline"` |

### Write (phone writes to characteristic)

The phone sends a CBOR map with an `"op"` field. The BLE service does **not**
decode writes itself. It copies the raw bytes to `_config_write_buf` under
`_config_write_mutex` and posts a `BleConfigWrite` event. The orchestrator
decodes and acts on it.

#### Set Config (orchestrator decodes)

```cbor
{"op": "set", "meas_int": 30, "temp_f": true}
```

Only changed keys are included. Omitted keys retain current values.

#### Execute Command (orchestrator decodes)

```cbor
{"op": "cmd", "cmd": "co2_cal"}
```

Supported commands (handled by orchestrator, not BLE service):

| `"cmd"` value | Action |
|---|---|
| `"co2_cal"` | Trigger CO2 background calibration |
| `"clear_data"` | Erase all stored route data from NAND |
| `"factory_rst"` | Reset all settings to defaults |

### Notify (server -> phone)

The Config characteristic sends two types of notifications, distinguished by
the `"type"` key:

#### Config Changed (`notify_config()`)

Sent after any configuration change is applied. Contains `"type": "config"`
plus all 13 config keys (the 12 from Read plus the discriminator):

```cbor
{"type": "config", "meas_int": 60, "pm_int": 10, ...all 12 keys...}
```

Implemented as inline CBOR encoding in `notify_config()` (13-key map: 1 type
discriminator + 12 config keys).

#### Command Result (`notify_command_result()`)

```cbor
{"type": "cmd_result", "cmd": "co2_cal", "ok": true}
```

On failure:

```cbor
{"type": "cmd_result", "cmd": "co2_cal", "ok": false, "err": "sensor_not_ready"}
```

Map size is 3 keys on success, 4 keys on failure (the `"err"` key is only
present when `ok` is false and a non-null error string is provided).

---

## Characteristic: History

Bulk export of stored route data from NAND flash. Uses a **stream with
selective retransmit** pattern: CBOR for control messages (tag `0x00`),
packed binary for bulk data (tag `0x01`).

### Notification Format

The first byte of every History notification is a type tag:

| Tag | Meaning | Remaining bytes |
|---|---|---|
| `0x00` | CBOR control response | CBOR-encoded map |
| `0x01` | Binary data chunk | `[uint16_le point_index][RoutePointWire...]` |

Implemented in `send_history_cbor()` and `send_history_binary()`.

### RoutePointWire Binary Format

55 bytes per point, packed little-endian. Converted from `RoutePoint` by
`route_point_to_wire()` using `memcpy` for type-punning safety.

| Offset | Size | Type | Field | Invalid sentinel |
|---|---|---|---|---|
| 0 | 4 | uint32_le | timestamp | — |
| 4 | 8 | float64_le | latitude | `GPS_LATITUDE_INVALID` |
| 12 | 8 | float64_le | longitude | `GPS_LONGITUDE_INVALID` |
| 20 | 4 | float32_le | altitude | `GPS_ALTITUDE_INVALID` |
| 24 | 1 | uint8 | gps_fix | raw enum value |
| 25 | 4 | float32_le | temperature | `MeasuresInvalid::TEMPERATURE` |
| 29 | 4 | float32_le | humidity | `MeasuresInvalid::HUMIDITY` |
| 33 | 4 | float32_le | pm1.0 | `MeasuresInvalid::PM` |
| 37 | 4 | float32_le | pm2.5 | `MeasuresInvalid::PM` |
| 41 | 4 | float32_le | pm10 | `MeasuresInvalid::PM` |
| 45 | 2 | int16_le | co2 | `-1` |
| 47 | 2 | int16_le | tvoc_index | `-1` |
| 49 | 2 | int16_le | nox_index | `-1` |
| 51 | 4 | float32_le | pressure | `MeasuresInvalid::PRESSURE` |

With 244-byte ATT payload: `(244 - 3) / 55 = 4` points per notification
(3 bytes for tag + point_index header).

### Write Commands (phone -> server)

All writes are CBOR maps with an `"op"` field. The BLE service copies raw
bytes to `_history_write_buf` under `_history_write_mutex` and posts a
`BleHistoryWrite` event. The orchestrator decodes and dispatches to the
appropriate `handle_history_*()` method.

#### List Sessions

```cbor
{"op": "list"}
```

#### Start Download

```cbor
{"op": "start", "session": 10042}
```

#### Fill Missing Points

```cbor
{"op": "fill", "pts": [12, 13, 14, 15, 78]}
```

The write buffer (256 bytes) fits approximately 50 point indices.

#### End Download

```cbor
{"op": "end"}
```

### Notify Responses (server -> phone)

#### Session List (`handle_history_list()`)

```cbor
{
  "type": "sessions",
  "sessions": [
    {"id": 10001, "pts": 150, "ts": 1737000000},
    {"id": 10002, "pts": 300, "ts": 1737100000}
  ]
}
```

Maximum 64 sessions (`MAX_SESSION_LIST`). Each session entry includes `"id"`
(session ID), `"pts"` (point count from `get_session_point_count()`), and
`"ts"` (start time from `get_session_start_time()`).

#### Download Started (`handle_history_start()`)

```cbor
{"type": "started", "session": 10042, "total": 300, "pt_size": 55}
```

`"pt_size"` is always 55 (`ROUTE_POINT_WIRE_SIZE`), allowing the phone to
verify wire format compatibility.

#### Download Done (after `handle_history_start()` or `handle_history_fill()`)

```cbor
{"type": "done", "sent": 300}
```

#### Download Ended (`handle_history_end()`)

```cbor
{"type": "ended"}
```

#### Error

```cbor
{"type": "error", "err": "session_not_found"}
```

| Error string | Cause | Sent by |
|---|---|---|
| `"session_not_found"` | Session ID does not exist (point count is 0) | `handle_history_start()` |
| `"no_active_download"` | `fill` received but `_export_active` is false | `handle_history_fill()` |
| `"flash_error"` | `read_route_points()` returned 0 during stream | `handle_history_start()` |

### Server-Side Pacing

The server streams notifications in a blocking loop. `notify()` returns
`false` when the TX buffer is full. The server retries with
`RTOS::delay_ms(1)` until space is available. This self-paces to the BLE
link speed.

During a stream, the orchestrator does not process other events. For typical
sessions (< 500 points, ~1-2 seconds at BLE 4.2 speeds), this is acceptable.

Implementation: `send_history_cbor()` and `send_history_binary()` both contain
the retry loop. They also check `_connected` on each retry and abort if the
client disconnected mid-stream.

### Storage Read Pattern

`handle_history_start()` reads points in batches of 4 (`ROUTE_READ_BATCH`),
converts each to wire format via `route_point_to_wire()`, and sends the
batch in a single binary notification. `handle_history_fill()` reads and
sends points one at a time.

### Download State Machine

```
         list (any state)
  +-----------------------------+
  |                             v
+------+  start    +---------------+  (stream completes)  +----------+
| Idle | --------> |  Streaming    | --------------------> |  Ready   |
|      |           |  (blocking)   |                       |          |
|      |           +---------------+                       |          |
|      | <-------------------------------------------------|          |
|      |  end / disconnect                         fill -> |          |
+------+                                           done -> |  (loop)  |
                                                           +----------+
```

- **Idle**: `_export_active = false`. Accepts `list` and `start`.
- **Streaming**: Server is in blocking loop. Transitions to Ready when done.
- **Ready**: `_export_active = true`. Accepts `fill`, `end`, `list`, `start`
  (new start implicitly ends current).
- **Disconnect**: `on_disconnect()` sets `_export_active = false`.

### Download Flow

```
Phone                              Device
  |                                  |
  |---- {"op": "list"} ------------>|
  |<---- [0x00] sessions -----------|
  |                                  |
  |---- {"op": "start", session: N} |
  |<---- [0x00] started ------------|
  |<---- [0x01] pts 0-3 -----------|  <- server streams all
  |<---- [0x01] pts 4-7 -----------|
  |      ... (notification lost) ...|
  |<---- [0x01] pts 16-19 ---------|
  |      ...                        |
  |<---- [0x01] pts 296-299 -------|
  |<---- [0x00] done (sent: 300) --|
  |                                  |
  |  (client detects gap: 12-15)    |
  |                                  |
  |---- {"op": "fill", pts:[12..15]}|
  |<---- [0x01] pts 12-15 ---------|
  |<---- [0x00] done (sent: 4) ----|
  |                                  |
  |---- {"op": "end"} ------------>|
  |<---- [0x00] ended --------------|
```

---

## API Reference

### Lifecycle

| Method | Description |
|---|---|
| `BleService(event_queue, storage)` | Constructor. `event_queue` is `RtosQueueHandle`, `storage` is `StorageService&`. |
| `init(serial)` | Init NimBLE, register GATT, configure security, start advertising. Returns `false` on failure. Uses a `static NimbleBleServer` instance. |
| `deinit()` | Stop advertising, disconnect, tear down. Resets all char pointers to `nullptr`, `_connected` to false, `_export_active` to false. Safe to call when not initialized. |

### Data Output (called by orchestrator)

| Method | Description |
|---|---|
| `notify_measures(measures, gps, timestamp)` | Encode via `encode_measures()`, `set_value()` + `notify()`. No-op if `!_connected` or `_measures_char == nullptr`. |
| `update_status(power, gps, tracking, session_id)` | Encode via `encode_status()`, `set_value()` only (read characteristic, no notification). |
| `update_config(settings)` | Encode via `encode_config()` (12 keys), `set_value()` only. |
| `notify_config(settings)` | Inline CBOR encoding (13 keys: 12 config + `"type"` discriminator), `set_value()` + `notify()`. |
| `notify_command_result(cmd, success, error)` | Inline CBOR encoding (3-4 keys), `set_value()` + `notify()`. |

### Pending Write Retrieval

| Method | Thread safety | Description |
|---|---|---|
| `take_pending_config_write(buf, buf_size)` | Locks `_config_write_mutex` | Copies raw CBOR, clears pending flag, returns byte count (0 if none pending). |
| `take_pending_history_write(buf, buf_size)` | Locks `_history_write_mutex` | Same pattern for history writes. |

### History Download

| Method | Blocking? | Description |
|---|---|---|
| `handle_history_list()` | No | Reads sessions from storage, sends CBOR session list notification. |
| `handle_history_start(session_id)` | **Yes** | Sends `"started"`, streams all points as binary, sends `"done"`. Aborts with `"error"` on flash failure. |
| `handle_history_fill(point_indices, count)` | **Yes** | Sends binary notifications for requested points, then `"done"`. |
| `handle_history_end()` | No | Sets `_export_active = false`, sends `"ended"`. |

### State Queries

| Method | Implementation | Description |
|---|---|---|
| `is_initialized()` | `_server != nullptr` | True after successful `init()`, false after `deinit()`. |
| `is_connected()` | `_connected.load()` | `std::atomic<bool>`, thread-safe. |

---

## Thread Safety

The BLE service straddles two task contexts:

| Context | Operations |
|---|---|
| **Orchestrator task** | `init()`, `deinit()`, `notify_*()`, `update_*()`, `take_pending_*()`, `handle_history_*()`, state queries |
| **NimBLE task** | `on_connect()`, `on_disconnect()`, `on_config_write()`, `on_history_write()`, `on_passkey_request()` |

### Synchronization Mechanisms

| Resource | Protection | Written by | Read by |
|---|---|---|---|
| `_config_write_buf` / `_config_write_len` / `_config_write_pending` | `_config_write_mutex` (`RtosMutex`) | NimBLE task (`on_config_write`) | Orchestrator (`take_pending_config_write`) |
| `_history_write_buf` / `_history_write_len` / `_history_write_pending` | `_history_write_mutex` (`RtosMutex`) | NimBLE task (`on_history_write`) | Orchestrator (`take_pending_history_write`) |
| `_connected` | `std::atomic<bool>` | NimBLE task (`on_connect`, `on_disconnect`) | Orchestrator (all `notify_*`, `handle_history_*`) |
| `_export_active`, `_export_session_id` | No mutex (single writer) | Orchestrator only | Orchestrator only |

NimBLE callbacks copy data to the pending buffer under the mutex, then post a
lightweight event (type only, no payload) to the orchestrator queue via
`RTOS::queue_send()`. The orchestrator retrieves data via `take_pending_*()`
under the same mutex.

---

## Power Management

When `operating_mode == Portable`, the device does not enter deep sleep or
light sleep. The orchestrator skips sleep evaluation entirely in Portable mode.
This keeps the BLE radio active, allowing persistent phone connections.

Continuous operation with BLE + sensors + GPS is power-intensive. The Status
characteristic provides real-time battery percentage for the app to display.

---

## MTU Handling

The implementation defines `MIN_USEFUL_MTU = 128`. If the negotiated MTU
is below this, a warning is logged. Notifications still attempt to send —
the NimBLE stack handles fragmentation for reads, but notifications that
exceed the ATT payload will be truncated.

Modern phones (iOS 7+, Android 5+) negotiate at least 185 bytes. An MTU
below 128 is unlikely in practice.

---

## Error Handling

| Scenario | Behavior | Location |
|---|---|---|
| BLE stack init fails | `init()` returns `false`, `_server` stays `nullptr` | `go_ble.cpp:112` |
| `set_security()` fails | `init()` returns `false` after `_server->deinit()` | `go_ble.cpp:119` |
| Any characteristic creation fails | `init()` returns `false` after cleanup | `go_ble.cpp:139-177` |
| Service `start()` fails | `init()` returns `false` after cleanup | `go_ble.cpp:186` |
| Advertising fails | `init()` returns `false` after cleanup | `go_ble.cpp:203-221` |
| Write callback with `len > WRITE_BUF_SIZE` | Logged, write silently dropped | `on_config_write`, `on_history_write` |
| Write callback with null data or zero length | Logged, write silently dropped | `on_config_write`, `on_history_write` |
| `notify_measures()` when not connected | No-op (early return) | `go_ble.cpp:382` |
| `encode_measures()` returns 0 | Warning logged, no notification sent | `go_ble.cpp:388` |
| History session not found (point count 0) | `"error": "session_not_found"` CBOR response | `handle_history_start()` |
| NAND read returns 0 points during stream | `"error": "flash_error"` CBOR response, `_export_active = false` | `handle_history_start()` |
| `fill` with no active download | `"error": "no_active_download"` CBOR response | `handle_history_fill()` |
| `notify()` returns false during stream | Retry with `RTOS::delay_ms(1)`, check `_connected` | `send_history_cbor`, `send_history_binary` |
| Client disconnects during stream | Retry loop detects `!_connected`, returns false | `send_history_cbor`, `send_history_binary` |
| Client disconnects at any time | `_connected = false`, `_export_active = false`, advertising restarts | `on_disconnect()` |

---

## Internal Constants

Defined as file-local `static constexpr` in `go_ble.cpp`:

| Constant | Value | Purpose |
|---|---|---|
| `CBOR_BUF_SIZE` | 256 | Stack buffer for all CBOR encoding |
| `WRITE_BUF_SIZE` | 256 | Pending write buffer size (class member) |
| `ROUTE_POINT_WIRE_SIZE` | 55 | Bytes per RoutePointWire |
| `BINARY_HEADER_SIZE` | 3 | Tag (1) + point_index (2) |
| `MAX_NOTIFY_PAYLOAD` | 244 | Conservative ATT payload limit |
| `POINTS_PER_NOTIFICATION` | 4 | `(244 - 3) / 55` |
| `ROUTE_READ_BATCH` | 4 | Points read from storage per iteration |
| `NOTIFY_RETRY_DELAY_MS` | 1 | Backpressure delay between retries |
| `MAX_SESSION_LIST` | 64 | Max sessions in a list response |
| `ADV_NAME_MAX_LEN` | 16 | Advertised name buffer size |
| `FW_VERSION` | `"0.0.0"` | Placeholder firmware version |

---

## StorageService Extensions Required

History export calls four methods that do not exist on `StorageService` yet:

```cpp
/// List all route session IDs on NAND.
uint16_t list_sessions(uint32_t *out, uint16_t max_count) const;

/// Get the number of route points in a session file.
uint32_t get_session_point_count(uint32_t session_id) const;

/// Read route points starting at offset.
uint16_t read_route_points(uint32_t session_id, uint32_t offset,
                           RoutePoint *out, uint16_t count) const;

/// Get the timestamp of the first route point.
time_t get_session_start_time(uint32_t session_id) const;
```

These are pure POSIX file operations on the existing route files. No format
changes needed — the sequential `RoutePoint` layout supports O(1) seeking.

---

## Orchestrator Integration (Not Yet Wired)

### Required Event Types

Add to `EventType` enum in `go_events.h`:

```cpp
BleConnected,      // no payload
BleDisconnected,   // no payload
BleConfigWrite,    // no payload (data in pending buffer)
BleHistoryWrite,   // no payload (data in pending buffer)
BlePairingRequest, // payload: uint32_t ble_passkey
```

Add to the `Event` union:

```cpp
uint32_t ble_passkey;  // BlePairingRequest
```

### Event Dispatch

| Event | Orchestrator Action |
|---|---|
| `BleConnected` | Set connected flag. Update display (BLE icon). Update status characteristic. |
| `BleDisconnected` | Clear connected flag. Update display. Call `handle_history_end()` if export active. |
| `BleConfigWrite` | `take_pending_config_write()` -> decode CBOR -> if `"set"`: validate, merge, save NVS, `notify_config()`. If `"cmd"`: execute, `notify_command_result()`. |
| `BleHistoryWrite` | `take_pending_history_write()` -> decode CBOR -> dispatch to `handle_history_list/start/fill/end()`. |
| `BlePairingRequest` | Render passkey on display (pairing overlay). |

### Mode Transitions

- **Entering Portable**: `ble_service.init(serial)`.
- **Leaving Portable**: `ble_service.deinit()`.

### Sensor Data Flow

```
SensorDataReady event
  +-> orchestrator stores _latest_measures
      +-> cache_measurement() (storage)
      +-> update display
      +-> if (ble_connected)
            ble_service.notify_measures(_latest_measures, _latest_gps, now)
```

### Settings Changed Flow (from BLE)

```
BleConfigWrite event
  +-> take_pending_config_write() -> raw CBOR
  +-> decode CBOR, extract "op"
  +-> if "set": validate fields, merge into GoSettings, save NVS
  |     +-> apply settings (reschedule timers, update sensor intervals)
  |     +-> ble_service.notify_config(settings)
  |     +-> ble_service.update_config(settings)
  +-> if "cmd": execute command
        +-> ble_service.notify_command_result(cmd, ok, err)
```

### Settings Changed Flow (from Display UI)

```
SettingsChanged event (existing)
  +-> orchestrator loads updated settings
      +-> apply settings
      +-> if (ble_connected)
      |     +-> ble_service.notify_config(settings)
      |     +-> ble_service.update_config(settings)
      +-> update display
```

---

## Build Configuration

### CMake

- `products/go/main/CMakeLists.txt`: `go_ble.cpp` in `SRCS`, `airgradient-ble` in `REQUIRES`
- `products/go/CMakeLists.txt`: `airgradient-ble` in `COMPONENTS`

### sdkconfig.defaults

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_NIMBLE_SM_LEGACY=y
CONFIG_BT_NIMBLE_SM_SC=y
```

### Additional Setup

- **TinyCBOR**: `idf.py -C products/go add-dependency "espressif/cbor^0.6.0~1"`
- **esp-nimble-cpp submodule**: `git submodule update --init`

---

## Known Compilation Blockers

The BLE service is implemented in isolation. It will not compile until the
following dependencies are resolved:

| Blocker | Why | Resolution |
|---|---|---|
| `StorageService` read methods | `handle_history_list()`, `handle_history_start()`, `handle_history_fill()` call `list_sessions()`, `get_session_point_count()`, `read_route_points()`, `get_session_start_time()` — declarations added to `go_storage.h`, implementations not yet written | Add implementations to `go_storage.cpp` |
| BLE event types | NimBLE callbacks post `BleConnected`, `BleDisconnected`, `BleConfigWrite`, `BleHistoryWrite`, `BlePairingRequest` — these are not in `go_events.h` yet | Add 5 event types and `uint32_t ble_passkey` union member. Event posting is commented out until then. |
| TinyCBOR dependency | `#include <cbor.h>` requires the managed component | Run `idf.py -C products/go add-dependency "espressif/cbor^0.6.0~1"` |
| esp-nimble-cpp submodule | `airgradient-ble` depends on it | Run `git submodule update --init` |

---

## Pending Integration Work

| Area | What |
|---|---|
| `go_events.h` | Add 5 BLE event types and `ble_passkey` union member |
| `go_orchestrator.h/.cpp` | Add `BleService` to `Services`, wire event dispatch, mode transitions, data forwarding |
| `go_storage.h/.cpp` | Add 4 read methods (see §StorageService Extensions Required) |
| `nand_storage.h` | Add `total_capacity_kb()` and `used_kb()` for Status flash reporting |
| `go_display.cpp` / `go_ui.cpp` | Passkey display overlay for `BlePairingRequest` events |
| `FW_VERSION` | Replace placeholder `"0.0.0"` with build-system-provided version |
| sdkconfig | Regenerate after adding NimBLE defaults |

---

## Testability

Only `init()` is guarded with `#ifndef TEST_HOST` (it instantiates the
concrete `NimbleBleServer`). All other methods use the abstract `AgBleServer*`
interface and compile under host tests.

### Host Tests

43 host tests in `products/go/tests/go_ble.tests.cpp` cover:

- **CBOR encoding**: `encode_measures()` (field omission, GPS inclusion),
  `encode_status()` (all 10 keys, battery clamping), `encode_config()`
  (12 keys), `notify_config()` (13 keys with type discriminator),
  `notify_command_result()` (success/failure variants)
- **Wire format**: `route_point_to_wire()` (55-byte layout, sentinel values)
- **String mapping**: `charging_state_to_str()`, `gps_mode_to_str()`,
  `operating_mode_to_str()` (all enum values)
- **Pending write buffers**: store/retrieve/reject/truncate for config and
  history writes
- **Notification flow**: no-op guards, `set_value()`/`notify()` behavior
- **Connection lifecycle**: connect/disconnect state transitions, advertising
- **History download**: list, start/error, stream, fill/error, end

Test infrastructure uses `BleServiceTestAccess` (friend class) to set private
state, `MockBleCharacteristic`/`MockBleServer` for capturing calls, and
`StorageService` stubs controlled via `storage_spy` namespace.

TinyCBOR is built as a native static library from the managed component
sources (pure C, no ESP-IDF dependency).

### Hardware Integration Testing

- Use a BLE scanner app (nRF Connect, LightBlue) to verify advertising,
  pairing, characteristic reads, and notifications.
- CBOR payloads can be decoded with `cbor.me` or any CBOR diagnostic tool.
- Verify the RoutePointWire format by downloading a known session and checking
  field values against the route file on NAND.
