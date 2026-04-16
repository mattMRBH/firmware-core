# AirGradient Go — BLE Client Integration Spec

Client-side specification for mobile app developers integrating with the
AirGradient Go (AGo) BLE peripheral. Covers discovery, pairing, GATT
operations, payload decoding, and the history download protocol.

---

## Table of Contents

1. [Discovery and Connection](#1-discovery-and-connection)
2. [Pairing and Security](#2-pairing-and-security)
3. [GATT Service Overview](#3-gatt-service-overview)
4. [Serialization](#4-serialization)
5. [Measures Characteristic](#5-measures-characteristic)
6. [Status Characteristic](#6-status-characteristic)
7. [Config Characteristic](#7-config-characteristic)
8. [History Characteristic](#8-history-characteristic)
9. [MTU Considerations](#9-mtu-considerations)
10. [Error Reference](#10-error-reference)
11. [Appendix: RoutePointWire Binary Format](#appendix-routepointwire-binary-format)
12. [Appendix: CBOR Quick Reference](#appendix-cbor-quick-reference)
13. [Appendix: Invalid Sentinel Values](#appendix-invalid-sentinel-values)

---

## 1. Discovery and Connection

### Advertised Name

The device advertises as **`AGo-<serial>`** where `<serial>` is a 12-character
lowercase hex string derived from the device's Wi-Fi MAC address.

Example: MAC `aa:bb:cc:dd:ee:ff` advertises as `AGo-aabbccddeeff`.

### Advertised Service UUID

The 128-bit service UUID is included in the advertising payload:

```
d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1
```

The complete local name is placed in the scan response data.

### Scanning Recommendations

- Filter by the service UUID above for reliable discovery.
- Alternatively, filter by name prefix `AGo-`.
- The device supports a single concurrent connection. While a client is
  connected, the device stops advertising. Advertising resumes after
  disconnect.

### Connection Parameters

The device is a BLE peripheral (Broadcaster + Peripheral roles). It does not
initiate connections.

---

## 2. Pairing and Security

### Pairing Model

The device uses **Passkey Entry** pairing with **Display Only** IO capability.
The device generates and displays a 6-digit numeric passkey (000000–999999)
on its e-paper screen. The user must enter this passkey on the phone to
complete pairing.

Security features:
- **Bonding**: Enabled. After successful pairing, the bond is stored and
  subsequent connections skip passkey entry.
- **MITM protection**: Enabled.

### Pairing Flow

```
Phone                              Device
  |                                  |
  |------- connect ----------------->|
  |                                  |
  |------- initiate pairing -------->|
  |                                  |
  |                         (generates passkey)
  |                         (shows passkey on display)
  |                                  |
  |  (user reads passkey from device)|
  |                                  |
  |------- enter passkey ----------->|
  |                                  |
  |<------- pairing complete --------|
  |                                  |
  |  (bonded — future connections    |
  |   skip passkey entry)            |
```

### Authenticated Characteristics

When security is enabled (production builds), the following characteristics
require an authenticated (paired) connection:

| Characteristic | Read | Write | Notify / Subscribe |
|---|---|---|---|
| Measures | — | — | Authenticated |
| Status | Authenticated | — | — |
| Config | Authenticated | Authenticated | — |
| History | — | Authenticated | — |

Attempting to access an authenticated characteristic, or subscribing to
Measures notifications, without pairing will usually trigger the BLE stack's
pairing flow automatically on supported platforms.

### Development Builds

Development firmware may disable BLE security. In this case, all
characteristics are accessible without pairing. The GATT structure remains
identical.

---

## 3. GATT Service Overview

### Service

| Field | Value |
|---|---|
| UUID | `d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1` |
| Type | Primary Service |

### Characteristics

| Name | UUID | Properties | Description |
|---|---|---|---|
| Measures | `d1c0c0a1-...` | Notify | Live sensor + GPS stream |
| Status | `d1c0c0a2-...` | Read | Device status snapshot |
| Config | `d1c0c0a3-...` | Read, Write, Notify | Get/set config, execute commands |
| History | `d1c0c0a4-...` | Write, Notify | Stored route data export |

All UUIDs share the base `d1c0c0aX-6b48-4b2a-9b1d-59f9f2b0a1e1` where `X`
is the characteristic index (1–4).

When BLE security is enabled, the Measures characteristic keeps its `Notify`
property but requires an authenticated connection before the subscription is
activated and notifications are delivered.

---

## 4. Serialization

All characteristic payloads use **CBOR** (RFC 8949, Concise Binary Object
Representation). CBOR is a compact binary encoding similar to JSON in
structure.

### Conventions

- **Map keys**: Short lowercase ASCII strings (e.g., `"t"`, `"pm25"`,
  `"co2"`).
- **Missing keys**: If a sensor reading is unavailable or invalid, the key
  is omitted entirely. A missing key means "data not available" — not zero.
- **Numeric types**:
  - Unsigned integers (`uint`): CO2, TVOC, NOx, timestamps, counts.
  - Float32 (`float`): Temperature, humidity, PM, pressure, altitude.
  - Float64 (`double`): GPS latitude and longitude (for precision).
  - Boolean: Config toggles, tracking state.
  - Text string: Enum values (charging state, GPS mode, etc.).

### Payload Sizes

All payloads fit within a single BLE notification (max ~253 bytes at standard
MTU). No application-level fragmentation is needed for CBOR payloads.

| Characteristic | Typical Size | Max Size |
|---|---|---|
| Measures | ~120 B | ~135 B |
| Status | ~110 B | ~130 B |
| Config (read) | ~120 B | ~150 B |
| Config (notify) | ~135 B | ~165 B |

---

## 5. Measures Characteristic

**UUID**: `d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Notify
**Direction**: Device -> Phone

### Subscription

Subscribe to notifications on this characteristic to receive live sensor
data. The device sends a notification each time a new measurement cycle
completes (typically every few seconds, configurable via Config).

In production builds, the subscription requires an authenticated (paired)
connection. If the client enables notifications before pairing, the BLE stack
may trigger the passkey pairing flow automatically and only begin delivering
Measures notifications after authentication succeeds.

Notifications are only sent while the device is in **Portable** operating
mode and the BLE client is connected.

### Payload

CBOR map. All keys are optional except `"ts"`.

| Key | Type | Unit | Description |
|---|---|---|---|
| `"t"` | float32 | Celsius | Temperature |
| `"h"` | float32 | % | Relative humidity |
| `"pm1"` | float32 | ug/m3 | PM 1.0 |
| `"pm25"` | float32 | ug/m3 | PM 2.5 |
| `"pm10"` | float32 | ug/m3 | PM 10 |
| `"co2"` | uint | ppm | CO2 concentration |
| `"tvoc"` | uint | index | TVOC index (1–500) |
| `"nox"` | uint | index | NOx index (1–500) |
| `"pres"` | float32 | hPa | Atmospheric pressure |
| `"lat"` | float64 | degrees | GPS latitude |
| `"lon"` | float64 | degrees | GPS longitude |
| `"alt"` | float32 | meters MSL | GPS altitude |
| `"fix"` | uint | enum | GPS fix type: 0=none, 2=2D, 3=3D |
| `"sat"` | uint | count | Satellite count |
| `"ts"` | uint | unix seconds | Measurement timestamp (**always present**) |

### GPS Field Rules

GPS fields (`"lat"`, `"lon"`, `"alt"`, `"fix"`, `"sat"`) are only included
when the device has a valid GPS fix. If the device is idle (not tracking) or
has no GPS fix, all GPS keys are absent.

When GPS is included:
- `"fix"` and `"sat"` are always present in the group.
- `"lat"`, `"lon"`, `"alt"` are individually omitted if that specific
  reading is invalid.

### Examples

**All sensors active, GPS has 3D fix:**

```json
{
  "t": 23.5, "h": 45.2,
  "pm1": 5.0, "pm25": 8.3, "pm10": 12.1,
  "co2": 450, "tvoc": 120, "nox": 5,
  "pres": 1013.2,
  "lat": 47.376887, "lon": 8.541694, "alt": 408.0,
  "fix": 3, "sat": 12,
  "ts": 1711234567
}
```

**Idle, CO2 sensor warming up:**

```json
{
  "t": 23.5, "h": 45.2,
  "pm1": 5.0, "pm25": 8.3, "pm10": 12.1,
  "tvoc": 120, "nox": 5,
  "pres": 1013.2,
  "ts": 1711234567
}
```

GPS keys absent (not tracking). `"co2"` key absent (sensor not ready).

---

## 6. Status Characteristic

**UUID**: `d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Read
**Direction**: Phone reads from device

### Usage

Read this characteristic at any time to get a snapshot of the device's
current status. The device updates this value internally after battery
polls, GPS fix changes, or tracking state changes.

### Payload

CBOR map. **All 10 keys are always present.**

| Key | Type | Description |
|---|---|---|
| `"gps_fix"` | uint | GPS fix type: `0` = none, `2` = 2D, `3` = 3D |
| `"gps_sat"` | uint | Satellite count (0 if unavailable) |
| `"bat_pct"` | uint | Battery percentage, 0–100 |
| `"bat_v"` | float32 | Battery voltage in Volts |
| `"charging"` | text | Charging state (see table below) |
| `"tracking"` | bool | `true` if a tracking session is active |
| `"session"` | uint | Current tracking session ID (0 if not tracking) |
| `"flash_kb"` | uint | Total flash storage capacity in KB |
| `"used_kb"` | uint | Used flash storage in KB |
| `"fw"` | text | Firmware version string |

### Charging State Values

| Value | Meaning |
|---|---|
| `"none"` | Not charging |
| `"trickle"` | Trickle charge |
| `"pre"` | Pre-charge |
| `"fast"` | Fast charge |
| `"taper"` | Taper charge |
| `"topoff"` | Top-off timer active |
| `"done"` | Charge complete |
| `"unknown"` | Unknown state |

### Example

```json
{
  "gps_fix": 3,
  "gps_sat": 12,
  "bat_pct": 72,
  "bat_v": 3.85,
  "charging": "none",
  "tracking": true,
  "session": 10042,
  "flash_kb": 262144,
  "used_kb": 8192,
  "fw": "3.2.1"
}
```

---

## 7. Config Characteristic

**UUID**: `d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Read, Write, Notify
**Direction**: Bidirectional

This characteristic supports three operations:

1. **Read** — Get current device configuration.
2. **Write** — Set config values or execute commands.
3. **Notify** — Receive config change confirmations and command results.

### 7.1 Read: Get Current Config

Read the characteristic to receive the full device configuration.

#### Payload (9-key CBOR map)

| Key | Type | Description |
|---|---|---|
| `"meas_int"` | uint | Measurement interval in seconds (1–3600). All sensors measured together at this cadence. |
| `"temp_f"` | bool | `true` = Fahrenheit, `false` = Celsius |
| `"pm_aqi"` | bool | `true` = US AQI for PM, `false` = raw ug/m3 |
| `"gps_int"` | uint | GPS update interval (seconds) |
| `"gps_mode"` | text | GPS mode (see table below) |
| `"inact_to"` | uint | Inactivity timeout (seconds) |
| `"auto_lock"` | uint | Auto-lock timeout (seconds) |
| `"dev_name"` | text | User-defined device name |
| `"op_mode"` | text | Operating mode (see table below) |

#### GPS Mode Values

| Value | Meaning |
|---|---|
| `"off"` | GPS always off |
| `"tracking"` | GPS on only during tracking |
| `"always"` | GPS always on |

#### Operating Mode Values

| Value | Meaning |
|---|---|
| `"portable"` | Portable mode (BLE active, battery powered) |
| `"stationary"` | Stationary mode (Wi-Fi connected, wall powered) |
| `"offline"` | Offline mode (no connectivity) |

#### Example

```json
{
  "meas_int": 10,
  "temp_f": false,
  "pm_aqi": false,
  "gps_int": 5,
  "gps_mode": "tracking",
  "inact_to": 300,
  "auto_lock": 60,
  "dev_name": "My AGo",
  "op_mode": "portable"
}
```

### 7.2 Write: Set Config

Write a CBOR map to update device configuration. Include `"op": "set"` and
only the keys you want to change. Omitted keys retain their current values.

#### Format

```json
{"op": "set", "meas_int": 30, "temp_f": true}
```

All config keys from the Read payload are supported. The `"op"` key is
required.

Deprecated keys (`"pm_int"`, `"other_int"`, `"disp_int"`) are accepted and
silently ignored for backward compatibility. They do not modify any setting.

#### Config key types for writes

| Key | Expected Type | Notes |
|---|---|---|
| `"meas_int"` | uint | 1–3600 seconds |
| `"temp_f"` | bool | |
| `"pm_aqi"` | bool | |
| `"gps_int"` | uint | |
| `"gps_mode"` | text | `"off"`, `"tracking"`, or `"always"` |
| `"inact_to"` | uint | |
| `"auto_lock"` | uint | |
| `"dev_name"` | text | Max 64 characters |
| `"op_mode"` | text | `"portable"`, `"stationary"`, or `"offline"` |

#### Response

After applying the config change, the device sends a **Config notification**
(see 7.4 below).

If the write contains any **unrecognized config key**, the entire write is
rejected. No settings are modified and the device sends an error
notification instead:

```json
{"type": "cmd_result", "cmd": "set", "ok": false, "err": "unknown_config_key"}
```

### 7.3 Write: Execute Command

Write a CBOR map to execute a device command.

#### Format

```json
{"op": "cmd", "cmd": "co2_cal"}
```

#### Supported Commands

| `"cmd"` Value | Action |
|---|---|
| `"co2_cal"` | Trigger CO2 sensor background calibration |
| `"clear_data"` | Erase all stored route data and clear chart cache |
| `"factory_rst"` | Reset to factory defaults: clears data, restores default settings, deletes BLE bonds, and reboots the device |
| `"start_tracking"` | Begin GPS + sensor route logging session |
| `"stop_tracking"` | End the current route logging session |
| `"set_aiding"` | Inject A-GNSS aiding data (position and/or time) to speed up GPS fix |

#### Response

The device sends a **command result notification** (see 7.5 below).

**Notes**:
- After `"factory_rst"`, the device reboots. The BLE connection will
  drop, and all bond information is erased. The phone will need to re-pair on
  the next connection.
- `"start_tracking"` fails with `"err": "already_tracking"` if a tracking
  session is already active.
- `"stop_tracking"` fails with `"err": "not_tracking"` if no tracking
  session is active.
- After a successful `"start_tracking"` or `"stop_tracking"`, the Status
  characteristic's `"tracking"` and `"session"` fields will reflect the new
  state on the next read.
- `"set_aiding"` accepts optional position and/or time fields (see below).
  At least one useful piece of data must be present; otherwise the command
  fails with `"err": "no_aiding_data"`.

#### Set Aiding Payload

The `"set_aiding"` command carries optional aiding data fields in the same
CBOR map. The device forwards valid data to the GPS module to reduce
cold-start time-to-first-fix (TTFF).

```json
{
  "op": "cmd",
  "cmd": "set_aiding",
  "lat": 47.376887,
  "lon": 8.541694,
  "alt": 408.0,
  "pos_acc": 50.0,
  "epoch": 1711234567,
  "time_acc": 2000
}
```

| Key | Type | Unit | Description | Required |
|---|---|---|---|---|
| `"lat"` | float64 | decimal degrees | Approximate latitude | No |
| `"lon"` | float64 | decimal degrees | Approximate longitude | No |
| `"alt"` | float32 | meters MSL | Approximate altitude | No |
| `"pos_acc"` | float32 | meters (1-sigma) | Position accuracy estimate | No |
| `"epoch"` | uint | POSIX seconds | Current UTC time | No |
| `"time_acc"` | uint | milliseconds | Time accuracy estimate | No |

**Position injection** requires both `"lat"` and `"lon"` to be present and
valid. `"alt"` and `"pos_acc"` are optional refinements.

**Time injection** requires `"epoch"` to be non-zero.

The phone should send this command shortly after connecting, using the
phone's own location and clock as the data source. It can also be re-sent
whenever the phone obtains a significantly updated position.

**System clock side-effect**: If the device's system clock has not yet been
synced (no GPS timestamp received), and the aiding data includes a valid
`"epoch"`, the device also sets its internal clock from the aiding epoch.
This provides meaningful timestamps for route-point data before the first
GPS fix arrives. The aiding epoch is approximate, so the device does not
mark the clock as synced — when a real GPS timestamp arrives, it overwrites
with the authoritative time.

### 7.4 Notify: Config Changed

Subscribe to Config notifications to receive confirmation when any
configuration change is applied (whether from this BLE client, another
source, or the device's own UI).

#### Payload (10-key CBOR map)

Same as the Read payload (9 config keys) plus a `"type"` discriminator:

```json
{"type": "config", "meas_int": 10, ...}
```

The `"type"` key distinguishes this from command result notifications (both
arrive on the same characteristic).

### 7.5 Notify: Command Result

After executing a command, the device sends a result notification.

#### Success

```json
{"type": "cmd_result", "cmd": "co2_cal", "ok": true}
```

3 keys: `"type"`, `"cmd"`, `"ok"`.

#### Failure

```json
{"type": "cmd_result", "cmd": "co2_cal", "ok": false, "err": "calibration_failed"}
```

4 keys: `"type"`, `"cmd"`, `"ok"`, `"err"`.

The `"err"` key is only present when `"ok"` is `false` and an error
description is available.

#### Command Error Strings

| Error String | Command | Cause |
|---|---|---|
| `"unsupported"` | `co2_cal` | CO2 sensor does not support calibration |
| `"calibration_failed"` | `co2_cal` | CO2 calibration procedure failed |
| `"clear_failed"` | `clear_data` | Route data erase did not complete fully |
| `"factory_reset_failed"` | `factory_rst` | Settings save, data clear, or bond delete failed |
| `"already_tracking"` | `start_tracking` | Tracking session was already active |
| `"not_tracking"` | `stop_tracking` | No tracking session was active |
| `"no_aiding_data"` | `set_aiding` | No valid position or time data in the payload |
| `"unknown_command"` | (any) | Unrecognised `"cmd"` string |
| `"unknown_config_key"` | `set` | Config write contained an unrecognised key; entire write rejected |

### 7.6 Notification Dispatch

Config notifications always contain a `"type"` key. Use it to dispatch:

| `"type"` Value | Notification Kind |
|---|---|
| `"config"` | Config changed — full config snapshot |
| `"cmd_result"` | Command result — check `"ok"` and `"err"` |

---

## 8. History Characteristic

**UUID**: `d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Write, Notify
**Direction**: Bidirectional

Bulk export of stored route session data from the device's flash storage.
This is a stateful download protocol using CBOR for control messages and
packed binary for bulk data transfer.

### 8.1 Notification Format

Every History notification starts with a **1-byte type tag**:

| Tag | Content | Meaning |
|---|---|---|
| `0x00` | CBOR map | Control response (session list, started, done, ended, error) |
| `0x01` | Binary data | Route point data chunk |

**Important**: Strip the first byte before decoding. CBOR payloads start at
byte index 1.

### 8.2 Write Commands (Phone -> Device)

All writes are CBOR maps with an `"op"` key.

#### List Sessions

```json
{"op": "list"}
```

Request the list of stored route sessions. Can be sent at any time.

#### Start Download

```json
{"op": "start", "session": 10042}
```

Start downloading all points for the specified session. The `"session"` value
is a session ID obtained from the session list.

#### Fill Missing Points

```json
{"op": "fill", "pts": [12, 13, 14, 15, 78]}
```

Request retransmission of specific point indices that were missed during the
initial download. The `"pts"` array contains zero-based point indices. Can
only be sent after a download has completed (after receiving `"done"`).

The device's write buffer (256 bytes) limits the `"pts"` array to
approximately 50 indices per request. For larger gaps, send multiple `fill`
requests.

#### End Download

```json
{"op": "end"}
```

Signal that the download is complete. Cleans up server-side state.

#### Delete Session

```json
{"op": "delete", "session": 10042}
```

Delete a single route session from the device's flash storage. The
`"session"` value is a session ID obtained from the session list.

The device rejects deletion of the currently active tracking session
(returns `"session_active"` error). If the session is currently being
downloaded, the device silently ends the export before deleting.

After a successful delete, the device updates its Status characteristic
internally so the next read reflects the reduced `"used_kb"`.

Can be sent at any time (does not require an active download).

### 8.3 Notify Responses (Device -> Phone)

All CBOR responses are prefixed with tag byte `0x00`. Decode the CBOR
starting at byte index 1.

#### Session List

Sent in response to `{"op": "list"}`. The response is **paginated** — the
device sends one notification per page. Each notification is self-contained
with pagination metadata. Collect pages until `"pg" == "tpg"`.

```json
{
  "type": "sessions",
  "sessions": [
    {"id": 10001, "pts": 150, "ts": 1737000000},
    {"id": 10002, "pts": 300, "ts": 1737100000}
  ],
  "pg": 1,
  "tpg": 3,
  "cnt": 13
}
```

| Field | Type | Description |
|---|---|---|
| `"id"` | uint | Session ID |
| `"pts"` | uint | Number of route points in this session |
| `"ts"` | uint | Session start time (unix seconds) |
| `"pg"` | uint | Current page number (1-based) |
| `"tpg"` | uint | Total number of pages |
| `"cnt"` | uint | Total number of sessions across all pages |

Sessions are paginated in groups of 6 per notification. There is no hard
cap on the total number of sessions — all sessions stored on the device
are included. If the device has no sessions, a single page is sent with
an empty `"sessions"` array and `"cnt": 0`.

#### Download Started

Sent at the beginning of a `start` operation, before binary data streaming
begins.

```json
{"type": "started", "session": 10042, "total": 300, "pt_size": 56}
```

| Field | Type | Description |
|---|---|---|
| `"session"` | uint | Session ID being downloaded |
| `"total"` | uint | Total number of points in this session |
| `"pt_size"` | uint | Bytes per route point (always 56) |

The `"pt_size"` field allows the client to verify wire format compatibility.
If `"pt_size"` is not 56, the client should abort — the binary format is
incompatible.

#### Binary Data Chunks

Sent after `"started"`. Tagged with `0x01`.

Binary layout:

```
[0x01] [uint16_le point_index] [RoutePointWire...] 
```

| Offset | Size | Type | Description |
|---|---|---|---|
| 0 | 1 | uint8 | Tag: `0x01` |
| 1 | 2 | uint16_le | Index of the first point in this chunk |
| 3 | N*56 | bytes | Packed RoutePointWire data |

Each notification contains up to **4 route points** (4 x 56 = 224 bytes of
point data, plus 3 bytes header = 227 bytes total).

Points are streamed sequentially starting from index 0. The `point_index`
field tells the client which points are in this notification. Use it to
detect gaps (missed notifications).

#### Download Done

Sent after all binary data has been streamed (for both `start` and `fill`
operations).

```json
{"type": "done", "sent": 300}
```

| Field | Type | Description |
|---|---|---|
| `"sent"` | uint | Number of points actually sent |

#### Download Ended

Sent in response to `{"op": "end"}`.

```json
{"type": "ended"}
```

#### Session Deleted

Sent in response to a successful `{"op": "delete"}`.

```json
{"type": "deleted", "session": 10042}
```

| Field | Type | Description |
|---|---|---|
| `"session"` | uint | Session ID that was deleted |

#### Error

Sent when an operation fails.

```json
{"type": "error", "err": "session_not_found"}
```

| Error String | Cause |
|---|---|
| `"session_not_found"` | The requested session ID does not exist |
| `"no_active_download"` | `fill` was sent without a prior `start` |
| `"flash_error"` | Flash storage read failed during streaming |
| `"delete_failed"` | Flash storage delete failed |
| `"session_active"` | Cannot delete the active tracking session |

### 8.4 Download Protocol Flow

#### Complete Download

```
Phone                              Device
  |                                  |
  |---- {"op": "list"} ------------>|
  |<-- [0x00] sessions list --------|
  |                                  |
  |---- {"op": "start", session: N} |
  |<-- [0x00] started --------------|
  |<-- [0x01] points 0-3 ----------|  (streaming begins)
  |<-- [0x01] points 4-7 ----------|
  |     ...                          |
  |<-- [0x01] points 296-299 ------|
  |<-- [0x00] done (sent: 300) ----|  (streaming complete)
  |                                  |
  |---- {"op": "end"} ------------>|
  |<-- [0x00] ended ----------------|
```

#### With Gap Recovery

BLE notifications are unreliable — they can be lost. The client should track
which point indices it received and use `fill` to request any gaps.

```
Phone                              Device
  |                                  |
  |---- {"op": "start", session: N} |
  |<-- [0x00] started --------------|
  |<-- [0x01] points 0-3 ----------|
  |<-- [0x01] points 4-7 ----------|
  |     ... (points 8-11 lost) ...   |
  |<-- [0x01] points 12-15 --------|
  |     ...                          |
  |<-- [0x00] done (sent: 300) ----|
  |                                  |
  |  (client detects gap: 8-11)      |
  |                                  |
  |---- {"op": "fill", pts:[8,9,10,11]} |
  |<-- [0x01] point 8 -------------|
  |<-- [0x01] point 9 -------------|
  |<-- [0x01] point 10 ------------|
  |<-- [0x01] point 11 ------------|
  |<-- [0x00] done (sent: 4) ------|
  |                                  |
  |  (verify all points received)    |
  |                                  |
  |---- {"op": "end"} ------------>|
  |<-- [0x00] ended ----------------|
```

#### Delete a Session

```
Phone                              Device
  |                                  |
  |---- {"op": "list"} ------------>|
  |<-- [0x00] sessions list --------|
  |                                  |
  |  (user selects session to delete)|
  |                                  |
  |---- {"op": "delete", session: N}|
  |<-- [0x00] deleted (session: N) -|
  |                                  |
  |  (optionally re-list to confirm) |
  |                                  |
  |---- {"op": "list"} ------------>|
  |<-- [0x00] sessions list --------|
```

If the session is currently being tracked:

```
Phone                              Device
  |                                  |
  |---- {"op": "delete", session: N}|
  |<-- [0x00] error: session_active |
```

### 8.5 Client Implementation Guide

1. **Subscribe** to History notifications before sending any write commands.
2. **List sessions** with `{"op": "list"}` to discover available data.
3. **Start download** with `{"op": "start", "session": <id>}`.
4. **Buffer incoming notifications** — dispatch by first byte tag:
   - `0x00`: Decode CBOR, handle control message.
   - `0x01`: Extract `point_index` (bytes 1-2, little-endian uint16), then
     parse route point data starting at byte 3.
5. **Track received indices** to detect gaps after `"done"` is received.
6. **Fill gaps** if any point indices are missing, using one or more `fill`
   commands.
7. **End download** with `{"op": "end"}` when all data is received.
8. **Delete sessions** with `{"op": "delete", "session": <id>}` to remove
   individual route files from the device. Check the Status characteristic's
   `"tracking"` and `"session"` fields first to avoid attempting to delete
   the active tracking session.

**Gap detection**: Compare the expected set of indices `[0, total-1]` with
the indices actually received. Each binary notification's `point_index` tells
you the starting index. With 4 points per notification, a notification with
`point_index = 8` contains points 8, 9, 10, 11.

---

## 9. MTU Considerations

- Modern phones (iOS 7+, Android 5+) negotiate at least 185-byte MTU.
- All CBOR payloads fit within 185 bytes.
- Binary history data chunks are 227 bytes maximum (3-byte header + 4 x 56
  bytes).
- Request an MTU of at least **251 bytes** during connection for optimal
  throughput on history downloads.
- The device logs a warning if the negotiated MTU is below 128 bytes.
  Notifications may be truncated at very low MTU values.

---

## 10. Error Reference

### Config Command Errors

Command result notifications (`"type": "cmd_result"`) may include an `"err"`
field. Error strings are operation-specific and defined by the device
firmware.

### History Protocol Errors

| Error | Meaning | Recovery |
|---|---|---|
| `"session_not_found"` | Session ID doesn't exist | Re-list sessions |
| `"no_active_download"` | `fill` sent without `start` | Send `start` first |
| `"flash_error"` | Storage read failed | Retry or skip session |
| `"delete_failed"` | Flash delete failed | Retry or ignore |
| `"session_active"` | Cannot delete the active tracking session | Stop tracking first via Config `"stop_tracking"` command, then retry |

### Connection Errors

- If the device disconnects during a history download, the download state is
  automatically cleaned up on the device side. The client must restart the
  download from `start` after reconnecting.
- The device is a single-connection peripheral. Only one phone can connect
  at a time.

---

## Appendix: RoutePointWire Binary Format

Each route point is 56 bytes, packed little-endian. Use this layout to parse
binary data from History notifications.

| Offset | Size | Type | Field | Invalid Sentinel |
|---|---|---|---|---|
| 0 | 4 | uint32_le | timestamp (unix seconds) | — |
| 4 | 8 | float64_le | latitude (degrees) | `91.0` |
| 12 | 8 | float64_le | longitude (degrees) | `181.0` |
| 20 | 4 | float32_le | altitude (meters MSL) | `-10000.0` |
| 24 | 1 | uint8 | gps_fix (0=none, 2=2D, 3=3D) | — |
| 25 | 4 | float32_le | temperature (Celsius) | `-1001.0` |
| 29 | 4 | float32_le | humidity (%) | `-1.0` |
| 33 | 4 | float32_le | pm1.0 (ug/m3) | `-1.0` |
| 37 | 4 | float32_le | pm2.5 (ug/m3) | `-1.0` |
| 41 | 4 | float32_le | pm10 (ug/m3) | `-1.0` |
| 45 | 2 | int16_le | co2 (ppm) | `-1` |
| 47 | 2 | int16_le | tvoc_index | `-1` |
| 49 | 2 | int16_le | nox_index | `-1` |
| 51 | 4 | float32_le | pressure (hPa) | `-1001.0` |
| 55 | 1 | uint8 | battery_percentage (0–100 %) | `255` |

**Total**: 56 bytes per point.

### Parsing Notes

- All multi-byte fields are **little-endian**.
- Check each field against its sentinel value to determine validity. If a
  field equals its sentinel, treat it as "not available."
- The `timestamp` field is always valid (no sentinel).
- The `gps_fix` field is always valid (0 means no fix).

---

## Appendix: CBOR Quick Reference

CBOR (RFC 8949) is a binary serialization format. Most platforms have CBOR
libraries available:

| Platform | Library |
|---|---|
| iOS/Swift | `SwiftCBOR`, `CBORCoding` |
| Android/Kotlin | `cbor-java`, `jackson-dataformat-cbor` |
| JavaScript/TypeScript | `cbor-x`, `cbor-web` |
| Python | `cbor2` |
| Dart/Flutter | `cbor` |

### Decoding Example (Pseudocode)

```
// Measures notification (no tag prefix)
raw_bytes = notification.value
map = cbor_decode(raw_bytes)
if "t" in map:
    temperature = map["t"]   // float32
if "co2" in map:
    co2 = map["co2"]         // uint

// History notification (has tag prefix)
raw_bytes = notification.value
tag = raw_bytes[0]
if tag == 0x00:
    map = cbor_decode(raw_bytes[1:])
    handle_control(map)
elif tag == 0x01:
    point_index = uint16_le(raw_bytes[1:3])
    point_data = raw_bytes[3:]
    parse_route_points(point_index, point_data)
```

---

## Appendix: Invalid Sentinel Values

These sentinel values indicate "data not available" in the binary route point
format. In CBOR payloads, unavailable fields are simply omitted (key absent).

| Field | Sentinel Value | Notes |
|---|---|---|
| latitude | `91.0` | Outside valid range [-90, 90] |
| longitude | `181.0` | Outside valid range [-180, 180] |
| altitude | `-10000.0` | Below minimum plausible altitude |
| temperature | `-1001.0` | Below absolute zero in any unit |
| humidity | `-1.0` | Below valid range [0, 100] |
| pm1.0 / pm2.5 / pm10 | `-1.0` | Negative PM is impossible |
| co2 | `-1` (int16) | Negative CO2 is impossible |
| tvoc_index | `-1` (int16) | Negative index is impossible |
| nox_index | `-1` (int16) | Negative index is impossible |
| pressure | `-1001.0` | Below valid atmospheric range |
| battery_percentage | `255` (uint8) | Outside valid range [0, 100] |
