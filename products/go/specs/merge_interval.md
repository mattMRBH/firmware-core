# Merge Interval — Implementation Spec

Replace the three independent interval settings (`pm_interval_seconds`,
`other_sensor_interval_seconds`, `display_refresh_interval_seconds`) with a
single `measure_interval_seconds`. This simplifies settings, UI, BLE
protocol, sleep cycle, and the orchestrator timer logic.

## Background

`GoSettings` currently declares three interval fields:

```cpp
int pm_interval_seconds = 10;           // 0 = PM sensor off
int other_sensor_interval_seconds = 10; // 0 = other sensors off
int display_refresh_interval_seconds = 60; // 0 = display off
```

These drive two independent sensor timers in the orchestrator (PM and
Other), each gated by its own interval. The display refresh interval
contributes to sleep duration calculation and controls the `display_off`
flag when set to 0.

In practice the three intervals add configuration complexity without clear
user benefit:

- **PM and Other** default to the same value (10s) and are almost always
  set identically.
- **Display refresh interval** does not actually control display refresh
  frequency in awake modes (Portable/Stationary) — the display refreshes
  on every `on_sensor_data()` event. It only affects: (1) the `display_off`
  flag when 0, and (2) the sleep duration floor in Offline mode.
- The independent-intervals spec (`independent_sensor_intervals.md`)
  enabled per-group on/off and staggered timing, but these capabilities are
  unused.

## Design Decisions

### Single Interval, Always Measuring

All sensors are measured together at the same cadence. Per-group on/off
(PM=0 or Other=0) is removed. The merged interval does not allow 0 — there
is no "Off" option.

Rationale: on AGo, measurement cost is ~50 ms per cycle (1 iteration).
Disabling individual sensor groups saves negligible power and adds UI/code
complexity.

### Display Off Kept as Code Path, Not Exposed in UI

`BuildContext::display_off` remains in the codebase but is always `false`.
No user-facing mechanism sets it. The display always shows sensor data when
there is data available. The code path can be re-enabled via a separate
boolean setting if needed in the future.

### BLE Key: Reuse `"meas_int"`

The legacy BLE key `"meas_int"` (currently skipped in decode) becomes the
active key for the merged interval. Old app versions that used the original
single-interval firmware will recognize this key and work seamlessly.

### NVS Key: Fresh `"mi"`

A fresh NVS key `"mi"` avoids loading stale values from any prior firmware.
The old key `"mis"` (removed in the independent-intervals spec) may still
exist on devices with value 60 — reusing it would override the intended
default of 10s. With `"mi"`, devices always start with the default.

Orphaned keys (`"pis"`, `"ois"`, `"dri"`, `"mis"`) are never read and
remain harmless in NVS.

### Deprecated BLE Keys: Skip Gracefully

When an old app sends `"pm_int"`, `"other_int"`, or `"disp_int"` in a
config write, the decoder matches them explicitly and skips the value
without modifying settings — the same pattern currently used for
`"meas_int"`. The config notify response to the client contains
`"meas_int"` and does not include the deprecated keys.

Note: the current BLE config-set flow has no per-key error response
mechanism. Config set decode returns `BleConfigOp::Set` without field-level
errors. To return an explicit "unsupported key" error, a new
`BleConfigDecodeResult` field and notify path would be needed — this is a
BLE protocol enhancement, not part of this spec. The current behavior
(silently skip, respond with current config) is sufficient for backward
compatibility.

### Default: 10 Seconds

Matches the current sensor interval defaults. Users who need longer
intervals can change it via UI or BLE.

### Interval Options

7 options, no "Off":

| Index | Label | Seconds |
|---|---|---|
| 0 | 1s | 1 |
| 1 | 10s | 10 |
| 2 | 30s | 30 |
| 3 | 60s | 60 |
| 4 | 5m | 300 |
| 5 | 15m | 900 |
| 6 | 1h | 3600 |

## Files

| File | Change |
|---|---|
| `go_settings.h` | Replace 3 interval fields with `measure_interval_seconds` |
| `go_settings.cpp` | Replace 3 NVS keys + validators with 1 |
| `go_orchestrator.h` | Replace 2 timer timestamps + group tracking with 1 timestamp |
| `go_orchestrator.cpp` | Single timer, simplified data merge, simplified sleep, simplified settings change |
| `go_power.cpp` | `decide_sleep()` uses single interval |
| `go_ui.h` | Replace 3 interval state vars with 1 |
| `go_ui.cpp` | Replace 3 settings rows + option arrays with 1, remove `is_display_off()` |
| `go_ble_protocol.h` | Remove deprecated key declarations (or keep commented) |
| `go_ble.cpp` | Encode/decode 1 key instead of 3, skip deprecated keys |
| `main.cpp` | Fast-path display: `display_off = false`, sleep uses single interval |
| `go_orchestrator.tests.cpp` | Rewrite timer/group/settings tests for single interval |
| `go_power.tests.cpp` | Rewrite sleep tests for single interval |
| `go_ui.tests.cpp` | Rewrite interval sync tests |
| `go_ble.tests.cpp` | Rewrite config encode/decode tests |

## GoSettings Changes

### go_settings.h

```cpp
// Remove:
int pm_interval_seconds = 10;
int other_sensor_interval_seconds = 10;
int display_refresh_interval_seconds = 60;

// Add:
int measure_interval_seconds = 10; // 1..3600
```

### go_settings.cpp

**Remove:**
- NVS keys `"pis"`, `"ois"`, `"dri"`
- `is_display_refresh_interval_valid()`
- `is_sensor_interval_valid()`
- Load/save logic for all three interval keys

**Add:**
- NVS key `"mi"`
- `is_measure_interval_valid()`: returns `true` if `1 <= value <= 3600`
- Load: read `"mi"` from ConfigStore. If missing or invalid, keep default (10).
- Save: validate + write `"mi"` to ConfigStore.

## Orchestrator Changes

### go_orchestrator.h

```cpp
// Remove:
uint32_t _last_pm_measurement_ms = 0;
uint32_t _last_other_measurement_ms = 0;
SensorGroup _last_requested_group = SensorGroup::None;

// Add:
uint32_t _last_measurement_ms = 0;
```

Rename `reschedule_sensor_timers(const GoSettings &)` →
`reschedule_sensor_timer(const GoSettings &)`.

### check_timers()

```
now = RTOS::get_time_ms()
interval = _settings.measure_interval_seconds * 1000

if (now - _last_measurement_ms) >= interval:
    _svc.sensor_producer.request_measurement(1, SensorGroup::All)
    _last_measurement_ms = now
```

Single timer, always `SensorGroup::All`. No group gating.

### compute_queue_timeout_ms()

Replace two sensor deadlines with one:

```
// Sensor timer deadline
deadline = _last_measurement_ms + (_settings.measure_interval_seconds * 1000)
remaining = deadline - now
next = min(next, remaining)

// BMS, inactivity deadlines: unchanged
```

### on_sensor_data()

Always overwrite all fields — no group-based gating:

```
on_sensor_data(data):
    _cached_measures.pm_a = data.pm_a
    _cached_measures.co2 = data.co2
    _cached_measures.temp_hum_a = data.temp_hum_a
    _cached_measures.tvoc_nox = data.tvoc_nox
    _cached_measures.pressure = data.pressure
    _cached_measures.power = data.power

    _first_measurement_done = true

    // Storage, tracking, BLE notify, display: unchanged
```

### reschedule_sensor_timer()

```
reschedule_sensor_timer(previous_settings):
    if previous_settings.measure_interval_seconds == _settings.measure_interval_seconds:
        return
    _last_measurement_ms = RTOS::get_time_ms()
```

### build_context()

```cpp
// Was:
.display_off = (_settings.display_refresh_interval_seconds == 0
                && _lock_state == LockState::Locked),

// Now:
.display_off = false,
```

### try_enter_sleep()

```cpp
// Was:
uint32_t awake_ms = now - std::min(_last_pm_measurement_ms, _last_other_measurement_ms);

// Now:
uint32_t awake_ms = now - _last_measurement_ms;
```

### init()

```
_last_measurement_ms = now
_svc.sensor_producer.request_measurement(1, SensorGroup::All)
```

### unlock()

Unchanged — already requests `SensorGroup::All`.

### apply_settings_change()

Calls `reschedule_sensor_timer(previous_settings)` instead of
`reschedule_sensor_timers(previous_settings)`.

### on_ble_config_write()

Same flow. `reschedule_sensor_timers()` → `reschedule_sensor_timer()`.

## Sleep Changes

### go_power.cpp — decide_sleep()

```cpp
// Was: min of three enabled intervals with UINT32_MAX fallback
// Now:
uint32_t interval_ms = static_cast<uint32_t>(settings.measure_interval_seconds) * 1000;
uint32_t sleep_ms = (awake_ms < interval_ms) ? (interval_ms - awake_ms) : 0;

if (sleep_ms >= deep_sleep_threshold_ms)
    return {SleepType::Deep, sleep_ms};
return {SleepType::None, 0};
```

No `UINT32_MAX` sentinel needed — `measure_interval_seconds` is always
≥ 1.

## UI Changes

### go_ui.h

```cpp
// Remove:
uint8_t _setting_display_interval = 1;
uint8_t _setting_pm_interval = 1;
uint8_t _setting_other_sensor = 1;

// Add:
uint8_t _setting_measure_interval = 1; // default index 1 = "10s"
```

Remove `is_display_off()` method.

### go_ui.cpp

**Remove option arrays:**

```cpp
// Remove:
static const char *const DISPLAY_INTERVAL_OPTIONS[] = {...};
static const char *const PM_INTERVAL_OPTIONS[] = {...};
static const char *const OTHER_SENSOR_OPTIONS[] = {...};
```

**Add option array:**

```cpp
static const char *const MEASURE_INTERVAL_OPTIONS[] = {
    "1s", "10s", "30s", "60s", "5m", "15m", "1h"};
static constexpr uint8_t MEASURE_INTERVAL_COUNT = 7;
```

**Setting row indices:**

```cpp
// Was:
static constexpr uint8_t SETTING_UNITS = 2;
static constexpr uint8_t SETTING_PM_DISPLAY = 3;
static constexpr uint8_t SETTING_DISPLAY_INTERVAL = 4;
static constexpr uint8_t SETTING_PM_INTERVAL = 5;
static constexpr uint8_t SETTING_OTHER_SENSOR = 6;
static constexpr uint8_t SETTING_GPS_MODE = 7;
static constexpr uint8_t SETTING_MODE = 8;
static constexpr uint8_t SETTING_AUTO_LOCK = 9;
static constexpr uint8_t SETTING_CLEAR_DATA = 10;
static constexpr uint8_t SETTING_SET_PMID = 11;
static constexpr uint8_t SETTINGS_TOTAL = 12;

// Now:
static constexpr uint8_t SETTING_UNITS = 2;
static constexpr uint8_t SETTING_PM_DISPLAY = 3;
static constexpr uint8_t SETTING_MEASURE_INTERVAL = 4;
static constexpr uint8_t SETTING_GPS_MODE = 5;
static constexpr uint8_t SETTING_MODE = 6;
static constexpr uint8_t SETTING_AUTO_LOCK = 7;
static constexpr uint8_t SETTING_CLEAR_DATA = 8;
static constexpr uint8_t SETTING_SET_PMID = 9;
static constexpr uint8_t SETTINGS_TOTAL = 10;
```

Net reduction: 3 rows removed, 1 added = -2 rows. `SETTINGS_TOTAL` goes
from 12 to 10.

**sync_settings():**

```cpp
// Was:
_setting_display_interval = seconds_to_index(s.display_refresh_interval_seconds, true);
_setting_pm_interval = seconds_to_index(s.pm_interval_seconds, true);
_setting_other_sensor = seconds_to_index(s.other_sensor_interval_seconds, true);

// Now:
_setting_measure_interval = seconds_to_index(s.measure_interval_seconds, false);
```

The `seconds_to_index` lambda's `has_off` parameter is `false` — there is
no Off option. If 0 is somehow passed, it clamps to index 0 ("1s").

Remove the `is_display_off()` check that resets `_active_metric`.

**apply_to_settings():**

```cpp
// Was:
settings.display_refresh_interval_seconds = index_to_seconds(_setting_display_interval);
settings.pm_interval_seconds = index_to_seconds(_setting_pm_interval);
settings.other_sensor_interval_seconds = index_to_seconds(_setting_other_sensor);

// Now:
settings.measure_interval_seconds = index_to_seconds(_setting_measure_interval);
```

The `index_to_seconds` lambda maps indices 0–6 to
`{1, 10, 30, 60, 300, 900, 3600}`. Index 7 (Off) is never reached since
`MEASURE_INTERVAL_COUNT` is 7.

**apply_setting_choice():**

Remove the `SETTING_DISPLAY_INTERVAL`, `SETTING_PM_INTERVAL`,
`SETTING_OTHER_SENSOR` cases. Add `SETTING_MEASURE_INTERVAL`:

```cpp
case SETTING_MEASURE_INTERVAL:
    _setting_measure_interval = option_index;
    break;
```

No `is_display_off()` reset logic — display is never off.

**setting_option_count() / setting_current_value() / build_values():**

Update all switch cases to use the new row indices and the single
`_setting_measure_interval` variable.

**Settings list label:**

The settings list renderer that draws row labels needs to show
"Measure Interval" (or "Measure Int.") at `SETTING_MEASURE_INTERVAL`
instead of the three old labels. Update the label array or switch
accordingly.

## BLE Changes

### go_ble_protocol.h

```cpp
// Active:
inline constexpr const char *BLE_KEY_MEAS_INT = "meas_int";

// Remove (or keep as deprecated comments):
// inline constexpr const char *BLE_KEY_PM_INT = "pm_int";
// inline constexpr const char *BLE_KEY_OTHER_INT = "other_int";
// inline constexpr const char *BLE_KEY_DISP_INT = "disp_int";
```

The deprecated key constants are still needed in `go_ble.cpp` for the skip
logic. Keep them declared (or inline the strings in the skip branches).

### go_ble.cpp — Encoding

**notify_config() (~line 509) and encode_config() (~line 1242):**

Replace 3 interval keys with 1:

```cpp
// Was:
cbor_encode_text_stringz(&map, BLE_KEY_PM_INT);
cbor_encode_uint(&map, settings.pm_interval_seconds);
cbor_encode_text_stringz(&map, BLE_KEY_OTHER_INT);
cbor_encode_uint(&map, settings.other_sensor_interval_seconds);
cbor_encode_text_stringz(&map, BLE_KEY_DISP_INT);
cbor_encode_uint(&map, settings.display_refresh_interval_seconds);

// Now:
cbor_encode_text_stringz(&map, BLE_KEY_MEAS_INT);
cbor_encode_uint(&map, settings.measure_interval_seconds);
```

Update CBOR map size: decrease by 2 (3 keys → 1).

### go_ble.cpp — Decoding

**decode_config_write() (~line 1462):**

```cpp
// "meas_int" — now active
else if (key_is(BLE_KEY_MEAS_INT)) {
    cbor_value_advance(&it);
    uint64_t v = 0;
    if (cbor_value_is_unsigned_integer(&it)
        && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.measure_interval_seconds = static_cast<uint32_t>(v);
    }
    handled = true;
}
// Deprecated keys — skip value, do not modify settings
else if (key_is(BLE_KEY_PM_INT)
         || key_is(BLE_KEY_OTHER_INT)
         || key_is(BLE_KEY_DISP_INT)) {
    cbor_value_advance(&it);
    handled = true;
}
```

## Fast-Path Boot Changes

### main.cpp

**build_fast_path_display():**

```cpp
// Was:
v.display_off = (settings.display_refresh_interval_seconds == 0);

// Now:
v.display_off = false;
```

**Sleep duration:**

The fast-path sleep duration calculation uses the single interval:

```cpp
// Was:
uint32_t interval_ms = UINT32_MAX;
if (settings.pm_interval_seconds > 0)
    interval_ms = min(interval_ms, settings.pm_interval_seconds * 1000);
if (settings.other_sensor_interval_seconds > 0)
    interval_ms = min(interval_ms, settings.other_sensor_interval_seconds * 1000);
if (settings.display_refresh_interval_seconds > 0)
    interval_ms = min(interval_ms, settings.display_refresh_interval_seconds * 1000);
if (interval_ms == UINT32_MAX)
    interval_ms = 60000;

// Now:
uint32_t interval_ms = settings.measure_interval_seconds * 1000;
```

## Edge Cases

| Case | Behavior |
|---|---|
| Interval = 1s | Measures every 1s. Deep sleep threshold (5s) not reached → stays awake in Offline mode. |
| Interval = 3600s (1h) | Deep sleep for ~3597s per cycle. |
| Old app sends `"pm_int"` / `"other_int"` / `"disp_int"` | Keys matched, values skipped. Settings unchanged. Config response contains `"meas_int"` — old keys absent. |
| Old app sends `"meas_int"` | Works — writes directly to `measure_interval_seconds`. |
| NVS has stale `"pis"` / `"ois"` / `"dri"` / `"mis"` | Never read. `"mi"` key missing → default 10s. |
| BLE client reads config after upgrade | Sees `"meas_int"` instead of 3 interval keys. |
| Display always shows data | `display_off` is always `false`. No "Display Off" UI option. |

## Testability

### Orchestrator Tests

| Scenario | Verify |
|---|---|
| Interval elapses | `request_measurement(1, SensorGroup::All)` called, timestamp updated |
| Interval not yet elapsed | No `request_measurement` call |
| `on_sensor_data` | All fields overwritten (PM, CO2, temp, TVOC, pressure) |
| `on_sensor_data` with sensor failure | Failed fields go to sentinel. All fields overwritten. |
| `compute_queue_timeout_ms` | Returns remaining time to sensor deadline |
| `init()` | Requests `SensorGroup::All`, sets timestamp |
| `unlock()` | Requests `SensorGroup::All` |
| Settings change same interval | `reschedule_sensor_timer` is a no-op |
| Settings change different interval | Baseline reset to now |
| BLE config write with `"meas_int"` | `_settings.measure_interval_seconds` updated, saved, UI synced |
| BLE config write with deprecated key | Settings unchanged, config notify sent |
| `build_context()` | `display_off == false` always |

### Sleep Tests

| Scenario | Verify |
|---|---|
| Offline + Locked + interval 60s, awake 3s | Sleep = 57000 ms, `SleepType::Deep` |
| Offline + Locked + interval 10s, awake 7s | Sleep = 3000 ms < 5000 threshold → `SleepType::None` |
| Offline + Locked + interval 3s, awake 1s | Sleep = 2000 ms < 5000 → `SleepType::None` |
| Portable mode | `SleepType::None` regardless of interval |
| Unlocked | `SleepType::None` regardless of interval |

### UI Tests

| Scenario | Verify |
|---|---|
| `sync_settings(10)` | `_setting_measure_interval` = 1 (index for "10s") |
| `sync_settings(300)` | `_setting_measure_interval` = 4 (index for "5m") |
| `apply_to_settings` with index 3 | `measure_interval_seconds` = 60 |
| Settings row count | `SETTINGS_TOTAL` = 10 |

### BLE Tests

| Scenario | Verify |
|---|---|
| Encode config | CBOR contains `"meas_int"`, no `"pm_int"` / `"other_int"` / `"disp_int"` |
| Decode `"meas_int": 30` | `settings.measure_interval_seconds` = 30 |
| Decode `"pm_int": 30` | Settings unchanged (key skipped) |
| Decode `"disp_int": 0` | Settings unchanged (key skipped) |

## What Is Not In This Spec

- No change to `SensorGroup` enum or `SensorManager` in shared components
- No change to `SensorProducer` interface (still accepts group parameter)
- No change to GPS interval (`gps_interval_seconds`)
- No change to display sleep optimization logic
- No per-key BLE error response for deprecated config keys (protocol
  enhancement — can be added separately if needed)
- No explicit NVS cleanup of orphaned keys (harmless, adds code for no
  benefit)
- No re-introduction of `display_off` UI toggle (future work)
