# BLE Service — Feature Spec

BLE peripheral service for the AirGradient Go product. Exposes sensor
measurements, device status, configuration, and stored route data to a
connected phone app over a single custom GATT service. Active only in
Portable operating mode.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_ble.h` | `BleService` class declaration |
| `products/go/main/go_ble.cpp` | GATT setup, CBOR encoding/decoding, callbacks |

Add `go_ble.cpp` to the `SRCS` list in `products/go/main/CMakeLists.txt`.
Uncomment `airgradient-ble` in `products/go/CMakeLists.txt` COMPONENTS list.

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `airgradient-ble` | component (`hal/ble_server.h`) | BLE HAL (BleServer, BleService, BleCharacteristic) |
| `NimbleBleServer` | component (`drivers/nimble_ble_server.h`) | Concrete NimBLE-backed BLE server |
| `esp-nimble-cpp` | component (git submodule) | NimBLE C++ wrapper (transitive via airgradient-ble) |
| `espressif/cbor` | ESP-IDF managed dependency (`^0.6.0~1`) | TinyCBOR encoder/decoder |
| `GoSettings` | product (`go_settings.h`) | Configuration struct |
| `MeasuresAGo` | common (`measures_types.h`) | Sensor measurement data |
| `GpsData` | gps (`gps_types.h`) | GPS position/fix data |
| `PowerSnapshot` | product (`go_power.h`) | Battery/charging status |
| `StorageService` | product (`go_storage.h`) | Route data read (requires extensions — see §14) |
| `Rtos::Queue` | common (`rtos.h`) | Event queue posting from callbacks |
| `Rtos::Mutex` | common (`rtos.h`) | Thread-safe access to pending write buffers |

### Build Configuration

Add TinyCBOR as a managed dependency for the Go product:

```bash
idf.py -C products/go add-dependency "espressif/cbor^0.6.0~1"
```

Add to `products/go/sdkconfig.defaults`:

```
# BLE (NimBLE)
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

---

## GATT Profile

### Service

| Field | Value |
|---|---|
| UUID | `d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1` |
| Type | Primary Service |

### Characteristics

| Name | UUID | Properties | Description |
|---|---|---|---|
| Measures | `d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1` | Notify | Live sensor + GPS stream |
| Status | `d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1` | Read | Device status snapshot |
| Config | `d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1` | Read, Write, Notify | Get/set config, execute commands |
| History | `d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1` | Write, Notify | Stored route data export |

---

## Advertising

| Field | Value |
|---|---|
| Device name | `AGo-<serial>` (serial derived from Wi-Fi station MAC) |
| Advertising data | AD Flags + 128-bit service UUID |
| Scan response | Complete Local Name |
| Interval | Default (100–500ms while undiscovered) |

The complete local name is placed in the scan response because the 128-bit
service UUID (18 bytes) plus AD flags (3 bytes) leaves only 10 bytes in the
31-byte advertising payload — insufficient for the full device name.

### Serial Derivation

Use the last 3 bytes of the Wi-Fi station MAC address, formatted as uppercase
hex. Example: MAC `AA:BB:CC:DD:EE:FF` → name `AGo-DDEEFF`.

---

## Security

### Pairing Model

Passkey Entry with Display Only IO capability. The BLE SMP specification
mandates a 6-digit numeric passkey (000000–999999); shorter PINs are not
supported by the standard pairing flow.

1. Device advertises, phone discovers and connects.
2. Phone initiates pairing.
3. NimBLE generates a random 6-digit passkey.
4. NimBLE invokes the passkey display callback in the BLE service.
5. BLE service posts a `BlePairingRequest` event (carrying the passkey) to the
   orchestrator queue.
6. Orchestrator renders the passkey on the e-paper display (pairing overlay).
7. User enters the passkey on the phone.
8. NimBLE completes the pairing handshake and establishes an encrypted link.
9. On success, NimBLE stores the bond in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST`).
10. BLE service posts a `BleConnected` event. Orchestrator dismisses the pairing
    overlay.

### Bonding

Bonded devices reconnect automatically without re-entering the passkey.
Bond data is persisted in NVS across power cycles. The phone can "forget" the
device to force re-pairing.

### NimBLE Security Configuration

```
IO capability:    Display Only
Authentication:   Bonding | MITM
Key distribution: Encryption key (LTK)
```

---

## Serialization

All characteristic payloads use **CBOR** (RFC 8949) encoded with **TinyCBOR**
(`espressif/cbor ^0.6.0~1`, added via `idf.py add-dependency` for the Go
product only).

### Conventions

- **Map keys**: Short lowercase strings (e.g., `"t"`, `"pm25"`). Balances
  compactness with debuggability.
- **Invalid fields**: Omit the key entirely. The phone app treats a missing key
  as "sensor not available / data not ready." This saves payload bytes and
  avoids exposing firmware-internal sentinel values.
- **Numeric types**: Integers for CO2/TVOC/NOx (whole numbers). Float32 for
  temperature, humidity, PM, pressure, altitude. Float64 for GPS latitude and
  longitude (full precision).
- **Encoding buffer**: Stack-allocated, sized to negotiated MTU. TinyCBOR's
  `CborEncoder` writes into a caller-provided buffer. No heap allocation
  during encoding.

### Estimated Payload Sizes

| Characteristic | Typical size | Max size | Fits in 253B ATT? |
|---|---|---|---|
| Measures | ~120B | ~135B | Yes |
| Status | ~110B | ~130B | Yes |
| Config (full) | ~140B | ~170B | Yes |
| History control (CBOR) | ~40B | ~180B | Yes |
| History data (binary, 4 pts) | 223B | 223B | Yes |

---

## Characteristic: Measures

### Trigger

When the orchestrator receives a `SensorDataReady` event and all of the
following are true:

- Operating mode is `Portable`
- A BLE client is connected
- The client has subscribed to Measures notifications (CCCD enabled)

The orchestrator calls `BleService::notify_measures()` with the latest
`MeasuresAGo`, `GpsData`, and system timestamp.

### CBOR Payload (Map)

| Key | CBOR Type | Source | Unit |
|---|---|---|---|
| `"t"` | float32 | `TempHumData::temperature` | °C |
| `"h"` | float32 | `TempHumData::humidity` | % |
| `"pm1"` | float32 | `PMData::pm_01` | µg/m³ |
| `"pm25"` | float32 | `PMData::pm_25` | µg/m³ |
| `"pm10"` | float32 | `PMData::pm_10` | µg/m³ |
| `"co2"` | uint | `CO2Data::co2` | ppm |
| `"tvoc"` | uint | `TVOCNOxData::tvoc_index` | index |
| `"nox"` | uint | `TVOCNOxData::nox_index` | index |
| `"pres"` | float32 | `PressureData::pressure` | hPa |
| `"lat"` | float64 | `GpsPosition::latitude` | decimal degrees |
| `"lon"` | float64 | `GpsPosition::longitude` | decimal degrees |
| `"alt"` | float32 | `GpsData::altitude_m` | meters MSL |
| `"fix"` | uint | `GpsFixType` | 0=none, 2=2D, 3=3D |
| `"sat"` | uint | `GpsFix::satellite_count` | count |
| `"ts"` | uint | system time | unix seconds |

**Always present**: `"ts"`.

**GPS fields (`"lat"`, `"lon"`, `"alt"`, `"fix"`, `"sat"`) are only included
when the device is in Tracking behavior.** When Idle (not tracking), GPS may be
off entirely (`GpsMode::OnWhenTracking`), so these keys are omitted from the
payload. This keeps the notification compact during casual monitoring.

**Omit when invalid** (using each field's `is_*_valid()` method): `"t"`, `"h"`,
`"pm1"`, `"pm25"`, `"pm10"`, `"co2"`, `"tvoc"`, `"nox"`, `"pres"`, `"lat"`,
`"lon"`, `"alt"`.

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

GPS keys are absent because the device is not tracking. CO2 key is absent
because `CO2Data::is_valid()` returned false.

---

## Characteristic: Status

### Trigger

Read-only. The BLE service updates the characteristic value whenever the
orchestrator calls `BleService::update_status()`. This happens:

- After each BMS poll (battery data changes)
- After GPS fix status changes
- After tracking starts/stops
- On any state change the status reflects

The phone app reads the value on demand; no notification.

### CBOR Payload (Map)

| Key | CBOR Type | Source | Description |
|---|---|---|---|
| `"gps_fix"` | uint | `GpsFixType` | 0=none, 2=2D, 3=3D |
| `"gps_sat"` | uint | `satellite_count` | Visible satellites |
| `"bat_pct"` | uint | `PowerSnapshot::battery_percentage` | 0–100 (%) |
| `"bat_v"` | float32 | `PowerSnapshot::battery_voltage` | Volts |
| `"charging"` | text | `BmsChargingState` | `"none"`, `"pre"`, `"fast"`, `"taper"`, `"done"`, `"unknown"` |
| `"tracking"` | bool | `RtcAppState::tracking_active` | Currently tracking? |
| `"session"` | uint | `RtcAppState::tracking_session_id` | 0 if not tracking |
| `"flash_kb"` | uint | NandStorage | Total flash capacity (KB) |
| `"used_kb"` | uint | NandStorage | Used flash (KB) |
| `"fw"` | text | build constant | Firmware version string |

All keys are always present. Battery fields use 0 / `"unknown"` when BMS has
not been polled yet (rather than omitting).

### Charging State Mapping

| `BmsChargingState` | CBOR text value |
|---|---|
| `Unknown` | `"unknown"` |
| `NotCharging` | `"none"` |
| `TrickleCharge` | `"trickle"` |
| `PreCharge` | `"pre"` |
| `FastCharge` | `"fast"` |
| `TaperCharge` | `"taper"` |
| `TopOffTimerActiveCharging` | `"topoff"` |
| `ChargeTerminationDone` | `"done"` |

---

## Characteristic: Config

Supports three operations through a single characteristic: **read current
config**, **set config values**, and **execute commands**.

### Read (phone reads characteristic)

Returns the full device configuration as a CBOR map. The BLE service keeps this
value updated whenever settings change (from BLE write, display UI, or boot).

#### CBOR Payload (Map)

| Key | CBOR Type | GoSettings field | Range | Default |
|---|---|---|---|---|
| `"meas_int"` | uint | `measurement_interval_seconds` | 1–3600 | 60 |
| `"pm_int"` | uint | `pm_interval_seconds` | 0–3600 | 10 |
| `"other_int"` | uint | `other_sensor_interval_seconds` | 0–3600 | 10 |
| `"disp_int"` | uint | `display_refresh_interval_seconds` | 0–3600 | 60 |
| `"temp_f"` | bool | `use_fahrenheit` | — | false |
| `"pm_aqi"` | bool | `pm_use_usaqi` | — | false |
| `"gps_int"` | uint | `gps_interval_seconds` | 1–60 | 5 |
| `"gps_mode"` | text | `gps_mode` | `"off"` / `"tracking"` / `"always"` | `"tracking"` |
| `"inact_to"` | uint | `inactivity_timeout_seconds` | 5–600 | 30 |
| `"auto_lock"` | uint | `auto_lock_seconds` | 0/10/30/60 | 0 |
| `"dev_name"` | text | `device_name` | 1–64 chars | `"airgradient-go"` |
| `"op_mode"` | text | `operating_mode` | `"portable"` / `"stationary"` / `"offline"` | `"offline"` |

### Write (phone writes to characteristic)

The phone sends a CBOR map with an `"op"` field to distinguish set-config from
execute-command.

#### Set Config

```cbor
{
  "op": "set",
  "meas_int": 30,
  "temp_f": true
}
```

- `"op": "set"` — identifies this as a config-set operation.
- Only include keys that are changing. Omitted keys retain their current values.
- The BLE service stores the raw CBOR bytes in an internal buffer and posts a
  `BleConfigWrite` event.
- The orchestrator decodes the CBOR, validates each field against `GoSettings`
  validation rules, merges into the current settings, persists to NVS, and
  applies.
- If validation fails for any field, that field is rejected silently (other
  valid fields in the same write still apply).
- After processing, the orchestrator calls `BleService::notify_config()`
  to notify the phone with the updated full config.

#### Execute Command

```cbor
{
  "op": "cmd",
  "cmd": "co2_cal"
}
```

- `"op": "cmd"` — identifies this as a command execution.
- `"cmd"` — command identifier string.
- The BLE service posts a `BleCommandReceived` event.
- The orchestrator executes the command and calls
  `BleService::notify_command_result()` to notify the phone with the result.

#### Supported Commands

| `"cmd"` value | Action | Notes |
|---|---|---|
| `"co2_cal"` | Trigger CO2 background calibration | Result: success/fail, may take several seconds |
| `"clear_data"` | Erase all stored route data from NAND | Irreversible |
| `"factory_rst"` | Reset all settings to defaults | Does not erase route data |

### Notify (server → phone)

The Config characteristic sends notifications for two reasons:

#### 1. Config Changed (from UI or BLE write)

Sent after any configuration change is applied, regardless of source:

```cbor
{
  "type": "config",
  "meas_int": 60,
  "pm_int": 10,
  ...full config map...
}
```

The `"type": "config"` discriminator tells the phone this is a config snapshot.
The payload contains all config keys (same schema as the Read payload).

#### 2. Command Result

Sent after a command completes:

```cbor
{
  "type": "cmd_result",
  "cmd": "co2_cal",
  "ok": true
}
```

On failure:

```cbor
{
  "type": "cmd_result",
  "cmd": "co2_cal",
  "ok": false,
  "err": "sensor_not_ready"
}
```

| Field | Type | Description |
|---|---|---|
| `"type"` | text | Always `"cmd_result"` |
| `"cmd"` | text | Echo of the command that was executed |
| `"ok"` | bool | Whether the command succeeded |
| `"err"` | text | Error description (present only when `"ok"` is false) |

---

## Characteristic: History

Bulk export of stored route data from NAND flash using a **stream with
selective retransmit** pattern. The server streams all route points as binary
notifications after a `start` command. The client tracks which points arrived
and requests retransmission of any missing points.

This hybrid approach uses CBOR for control messages (infrequent, flexibility
matters) and packed binary for data transfer (bulk, efficiency matters).

### Notification Format

The History characteristic multiplexes CBOR control and binary data on the
same Notify channel. The first byte of every notification is a type tag:

| Tag | Meaning | Remaining bytes |
|---|---|---|
| `0x00` | CBOR control response | CBOR-encoded map |
| `0x01` | Binary data chunk | `[uint16_le point_index][RoutePointWire...]` |

### RoutePointWire Binary Format

Packed, little-endian, 55 bytes per point. This is the wire format — not the
internal `RoutePoint` struct. The server converts field-by-field when reading
from NAND.

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | uint32_le | timestamp (unix seconds) |
| 4 | 8 | float64_le | latitude (degrees) |
| 12 | 8 | float64_le | longitude (degrees) |
| 20 | 4 | float32_le | altitude (meters MSL) |
| 24 | 1 | uint8 | gps_fix (0=none, 2=2D, 3=3D) |
| 25 | 4 | float32_le | temperature (°C) |
| 29 | 4 | float32_le | humidity (%) |
| 33 | 4 | float32_le | pm1.0 (µg/m³) |
| 37 | 4 | float32_le | pm2.5 (µg/m³) |
| 41 | 4 | float32_le | pm10 (µg/m³) |
| 45 | 2 | int16_le | co2 (ppm) |
| 47 | 2 | int16_le | tvoc_index |
| 49 | 2 | int16_le | nox_index |
| 51 | 4 | float32_le | pressure (hPa) |

Invalid fields use the corresponding `MeasuresInvalid` / `GPS_*_INVALID`
sentinel values cast to the wire type (e.g., `-1.0f` for invalid floats,
`-1` for invalid int16 fields).

With 244-byte ATT payload: `(244 - 3) / 55 = 4` points per notification
(3 bytes for tag + point_index). 300 points = 75 notifications.

### Write Commands (phone → server)

All writes are CBOR maps with an `"op"` field.

#### List Sessions

```cbor
{"op": "list"}
```

Request a list of all stored route sessions. Works in any state.

#### Start Download

```cbor
{"op": "start", "session": 10042}
```

Begin streaming a session. If another download is already active, it is
implicitly ended first.

The server responds with a CBOR `"started"` notification, then immediately
begins streaming all route points as binary data notifications. After all
points are sent, the server sends a CBOR `"done"` notification.

#### Fill Missing Points

```cbor
{"op": "fill", "pts": [12, 13, 14, 15, 78]}
```

Request retransmission of specific points by index. Sent after the client
receives the `"done"` notification and detects gaps.

- `"pts"` — array of zero-based point indices to retransmit.

The server responds with binary data notifications for the requested points
(possibly batched into multiple notifications), followed by a `"done"`
notification.

The write buffer (256 bytes) fits approximately 50 point indices in CBOR. If
more points are missing, the client can send multiple `fill` commands, or
re-start the entire download.

#### End Download

```cbor
{"op": "end"}
```

Close the current download. Safe to call when no download is active (no-op).

### Notify Responses (server → phone)

#### CBOR Control Responses (tag `0x00`)

**Session List:**

```cbor
{
  "type": "sessions",
  "sessions": [
    {"id": 10001, "pts": 150, "ts": 1737000000},
    {"id": 10002, "pts": 300, "ts": 1737100000}
  ]
}
```

- `"id"` — session ID (uint, 10000–99999).
- `"pts"` — number of route points in the session.
- `"ts"` — unix timestamp of the first point. 0 if empty.

If the session list exceeds the MTU, it is split across multiple notifications
with a `"more": true` flag. The last chunk omits `"more"`.

**Download Started:**

```cbor
{
  "type": "started",
  "session": 10042,
  "total": 300,
  "pt_size": 55
}
```

- `"session"` — echoed session ID.
- `"total"` — total number of route points.
- `"pt_size"` — size of one `RoutePointWire` in bytes (55). Allows the phone
  to pre-allocate a receive buffer and verify wire format compatibility.

**Download Done:**

```cbor
{
  "type": "done",
  "sent": 300
}
```

- `"sent"` — number of points the server transmitted. The client compares this
  against the number of unique point indices it actually received. Any gaps are
  requested via `fill`.

**Download Ended:**

```cbor
{"type": "ended"}
```

Confirms the download session is closed.

**Error:**

```cbor
{"type": "error", "err": "session_not_found"}
```

| Error string | Cause |
|---|---|
| `"session_not_found"` | Requested session ID does not exist |
| `"no_active_download"` | `fill` received but no download is active |
| `"flash_error"` | NAND read failure during stream |

#### Binary Data Notifications (tag `0x01`)

```
[0x01] [uint16_le first_point_index] [RoutePointWire] [RoutePointWire] ...
```

Points are sequential starting from `first_point_index`. The number of points
is implicit: `(notification_length - 3) / 55`.

The client tracks received point indices. If a notification carrying points
12–15 is lost, the client detects the gap after `"done"` and sends
`{"op": "fill", "pts": [12, 13, 14, 15]}`.

### Server-Side Pacing

The server streams notifications in a blocking loop within the orchestrator
task. NimBLE's `notify()` returns `false` when the TX buffer is full. The
server retries with a short delay (`RTOS::delay_ms(1)`) until buffer space is
available. This self-paces to the BLE link speed.

During a stream, the orchestrator does not process other events. For typical
sessions (< 500 points, ~1–2 seconds), this is acceptable. Sensor and input
events queue up and are processed after the stream completes.

### Download Flow

```
Phone                              Device
  │                                  │
  │──── {"op": "list"} ─────────────►│
  │◄──── [0x00] sessions ───────────│
  │                                  │
  │──── {"op": "start", session: N} ►│
  │◄──── [0x00] started ────────────│
  │◄──── [0x01] pts 0-3 ────────────│  ← server streams all
  │◄──── [0x01] pts 4-7 ────────────│
  │      ... (notification lost) ... │
  │◄──── [0x01] pts 16-19 ──────────│
  │      ...                         │
  │◄──── [0x01] pts 296-299 ────────│
  │◄──── [0x00] done (sent: 300) ───│
  │                                  │
  │  (client detects gap: 12-15)     │
  │                                  │
  │──── {"op": "fill", pts: [12..15]}►│
  │◄──── [0x01] pts 12-15 ──────────│
  │◄──── [0x00] done (sent: 4) ─────│
  │                                  │
  │──── {"op": "end"} ──────────────►│
  │◄──── [0x00] ended ──────────────│
```

### Download State Machine

```
         list (any state)
  ┌─────────────────────────────┐
  │                             ▼
┌──────┐  start    ┌───────────────┐  (stream completes)  ┌──────────┐
│ Idle │ ────────► │  Streaming    │ ────────────────────► │  Ready   │
│      │           │  (blocking)   │                       │          │
│      │           └───────────────┘                       │          │
│      │ ◄─────────────────────────────────────────────────│          │
│      │  end / disconnect                        fill ──► │          │
└──────┘                                          done ──► │  (loop)  │
                                                           └──────────┘
```

- **Idle**: No download in progress. Accepts `list` and `start`.
- **Streaming**: Server is actively sending binary notifications. Blocks the
  orchestrator. Transitions to Ready when all points are sent.
- **Ready**: Stream complete. Accepts `fill` (retransmit), `end` (close),
  `list`, and `start` (new download, implicitly ends current).
- **Disconnect**: Any state transitions to Idle (release file handle).

---

## Class Design

```cpp
#pragma once

#include "go_events.h"
#include "go_settings.h"
#include "go_types.h"
#include "hal/ble_server.h"
#include "measures_types.h"
#include "rtos.h"
#include "types/gps_types.h"

#include <cstdint>

class StorageService; // forward declaration

class BleService {
public:
  // --- Construction ---

  /// event_queue: shared orchestrator event queue (for posting BLE events)
  /// storage: storage service reference (for history export reads)
  explicit BleService(Rtos::Queue<Event> &event_queue, StorageService &storage);

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
  void update_status(const PowerSnapshot &power, const GpsData &gps,
                     bool tracking_active, uint32_t session_id);

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

  /// Process decoded history commands. The orchestrator decodes the CBOR
  /// from take_pending_history_write() and calls the appropriate method.

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

private:
  Rtos::Queue<Event> &_event_queue;
  StorageService &_storage;

  BleServer *_server = nullptr;
  BleCharacteristic *_measures_char = nullptr;
  BleCharacteristic *_status_char = nullptr;
  BleCharacteristic *_config_char = nullptr;
  BleCharacteristic *_history_char = nullptr;

  bool _connected = false;

  // --- Pending write buffers (written by NimBLE callbacks, read by orchestrator) ---

  static constexpr size_t WRITE_BUF_SIZE = 256;

  Rtos::Mutex _config_write_mutex;
  uint8_t _config_write_buf[WRITE_BUF_SIZE] = {};
  size_t _config_write_len = 0;
  bool _config_write_pending = false;

  Rtos::Mutex _history_write_mutex;
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
  size_t encode_measures(uint8_t *buf, size_t buf_size,
                         const MeasuresAGo &m, const GpsData &gps, time_t ts);
  size_t encode_status(uint8_t *buf, size_t buf_size,
                       const PowerSnapshot &power, const GpsData &gps,
                       bool tracking, uint32_t session_id);
  size_t encode_config(uint8_t *buf, size_t buf_size, const GoSettings &settings);
};
```

### Thread Safety

The BLE service straddles two task contexts:

| Context | Operations |
|---|---|
| **Orchestrator task** | `init()`, `deinit()`, `notify_*()`, `update_*()`, `take_pending_*()`, `handle_history_*()`, state queries |
| **NimBLE task** | `on_connect()`, `on_disconnect()`, `on_config_write()`, `on_history_write()`, `on_passkey_request()` |

The `_config_write_mutex` and `_history_write_mutex` protect the pending write
buffers. The `_connected` flag is written from the NimBLE task and read from the
orchestrator — use `std::atomic<bool>` or protect with a mutex.

Callbacks copy data to the pending buffer (under mutex), then post a
lightweight event (type only, no payload) to the orchestrator queue.

---

## Orchestrator Integration

### New Event Types

Add to `EventType` enum in `go_events.h`:

```cpp
// --- BLE events ---
BleConnected,        // no payload
BleDisconnected,     // no payload
BleConfigWrite,      // no payload (data in BleService pending buffer)
BleHistoryWrite,     // no payload (data in BleService pending buffer)
BlePairingRequest,   // payload: uint32_t passkey
```

Add to the `Event` union:

```cpp
uint32_t ble_passkey;  // BlePairingRequest
```

### Event Dispatch

| Event | Orchestrator Action |
|---|---|
| `BleConnected` | Set `_ble_connected = true`. Update display (BLE icon). Update status characteristic. |
| `BleDisconnected` | Set `_ble_connected = false`. Update display. Clean up any active history export (`handle_history_end()`). |
| `BleConfigWrite` | Call `take_pending_config_write()`. Decode CBOR. If `"op": "set"`: validate, merge into `GoSettings`, save to NVS, apply, call `notify_config()`. If `"op": "cmd"`: execute command, call `notify_command_result()`. |
| `BleHistoryWrite` | Call `take_pending_history_write()`. Decode CBOR. Dispatch to `handle_history_list()`, `handle_history_start()`, `handle_history_fill()`, or `handle_history_end()`. Note: `start` and `fill` block the orchestrator during the binary stream. |
| `BlePairingRequest` | Show passkey on display (pairing overlay or snackbar). |

### Mode Transitions

In `Orchestrator::change_mode()`:

- **Entering Portable**: Call `_ble_service.init(serial)`. The BLE stack
  initializes and advertising begins.
- **Leaving Portable**: Call `_ble_service.deinit()`. The BLE stack is torn
  down, client is disconnected.

On boot, if `GoSettings::operating_mode == Portable`, call `init()` during the
boot sequence (after NVS and I2C are ready).

### Sensor Data Flow (Portable Mode)

```
SensorDataReady event
  └─► orchestrator stores _latest_measures
      ├─► cache_measurement() (storage)
      ├─► update display
      └─► if (ble_connected && measures_subscribed)
            ble_service.notify_measures(_latest_measures, _latest_gps, now)
```

### Settings Changed Flow (from BLE)

```
BleConfigWrite event
  └─► orchestrator
      ├─► take_pending_config_write() → raw CBOR
      ├─► decode CBOR, extract "op"
      ├─► if "set": validate fields, merge into GoSettings, save NVS
      │     ├─► apply settings (reschedule timers, update sensor intervals)
      │     ├─► ble_service.notify_config(settings)
      │     └─► ble_service.update_config(settings)
      └─► if "cmd": execute command
            └─► ble_service.notify_command_result(cmd, ok, err)
```

### Settings Changed Flow (from Display UI)

```
SettingsChanged event (existing)
  └─► orchestrator
      ├─► load_go_settings() → updated settings
      ├─► apply settings
      ├─► if (ble_connected)
      │     ├─► ble_service.notify_config(settings)
      │     └─► ble_service.update_config(settings)
      └─► update display
```

---

## StorageService Extensions

History export requires new read capabilities on `StorageService`. These do not
exist in the current implementation and must be added as a prerequisite.

### New Methods

```cpp
/// List all route session IDs on NAND. Scans the routes/ directory.
/// Writes session IDs to out[], sorted ascending. Returns the number found
/// (up to max_count).
uint16_t list_sessions(uint32_t *out, uint16_t max_count) const;

/// Get the number of route points in a session file.
/// Returns 0 if the session does not exist or the file is empty.
uint32_t get_session_point_count(uint32_t session_id) const;

/// Read route points from a session file.
/// Reads up to count points starting at point index offset.
/// Returns the number of points actually read.
uint16_t read_route_points(uint32_t session_id, uint32_t offset,
                           RoutePoint *out, uint16_t count) const;

/// Get the timestamp of the first route point in a session.
/// Returns 0 if the session is empty or does not exist.
time_t get_session_start_time(uint32_t session_id) const;
```

These are pure POSIX file operations (`fopen`, `fseek`, `fread`) on the
existing route files. No format changes needed — the sequential `RoutePoint`
layout supports O(1) seeking by index.

---

## Power Management

### Portable Mode: No Sleep

When `operating_mode == Portable`, the device does not enter deep sleep or
light sleep. The orchestrator skips `evaluate_sleep()` entirely in Portable
mode.

This keeps the BLE radio active continuously, allowing the phone to maintain
a persistent connection and receive live measurement notifications.

### Battery Impact

Continuous operation with BLE radio + sensors + GPS is power-intensive. The
phone app should inform the user of expected battery life. The Status
characteristic provides real-time battery percentage for the app to display.

### Future Improvement

A future iteration may refine this to: suppress deep sleep only while a BLE
client is connected (or subscribed to notifications). When no client is
connected, allow normal sleep behavior. This would extend battery life in
Portable mode when the phone app is not actively used.

---

## MTU Handling

### Negotiation

The BLE service requests a 256-byte MTU during connection setup. The actual
negotiated MTU depends on the phone.

### Minimum Supported MTU

If the negotiated MTU is less than 128 bytes, the BLE service logs a warning.
Notification-based characteristics (Measures, Config notify, History data)
will not send notifications because the CBOR payloads cannot fit. Read
characteristics (Status, Config read) still work via BLE Long Reads (automatic
ATT fragmentation handled by the NimBLE stack).

Modern phones (iOS 7+, Android 5+) negotiate at least 185 bytes. A negotiated
MTU below 128 is unlikely in practice.

---

## Error Handling

| Scenario | Behavior |
|---|---|
| BLE stack init fails | `init()` returns false. Orchestrator logs error. BLE remains disabled. |
| Advertising fails to start | `init()` returns false after cleanup. |
| Notification send fails (no subscribers) | `notify()` returns false. Silently ignored. |
| Client writes invalid CBOR | BLE service cannot decode in orchestrator. No response sent. Log warning. |
| Client writes unknown `"op"` | Ignored. Log warning. |
| Client writes unknown `"cmd"` | Respond with `{"type": "cmd_result", "cmd": "...", "ok": false, "err": "unknown_command"}`. |
| Config validation fails | Individual invalid fields are silently skipped. Valid fields in the same write still apply. |
| History session not found | Respond with `[0x00]{"type": "error", "err": "session_not_found"}`. |
| NAND read failure during stream | Respond with `[0x00]{"type": "error", "err": "flash_error"}`. Abort stream, transition to Idle. |
| `fill` with no active download | Respond with `[0x00]{"type": "error", "err": "no_active_download"}`. |
| `notify()` returns false during stream | Retry with `RTOS::delay_ms(1)` until buffer drains. Self-paces to link speed. |
| Client disconnects during download | BLE service transitions to Idle. File handle released on `BleDisconnected` event. |
| Passkey entry timeout | NimBLE handles timeout internally. Pairing fails. Client can retry. |

---

## Open Questions When Implementing

1. **Notification interval throttling**: Should the BLE service throttle
   Measures notifications to a minimum interval, independent of the
   measurement interval? (Relevant if measurement interval is set very low,
   e.g., 1 second.)

2. **Config write ACK**: Should the Config characteristic respond to a set-config
   write with a write-response status code (ATT error response for invalid
   writes)? Current design always accepts the write and applies valid fields,
   notifying the result. An ATT-level error would let the phone know the write
   was malformed before the notification arrives.
