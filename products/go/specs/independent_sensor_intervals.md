# Independent PM and Other-Sensor Intervals — Implementation Spec

Replace the single `measurement_interval_seconds` timer with two independent
measurement cycles: one for PM sensors and one for all other sensors (CO2,
TVOC/NOx, temp/hum, pressure). The two dead settings `pm_interval_seconds`
and `other_sensor_interval_seconds` become the active runtime intervals.

## Background

`GoSettings` declares three interval fields:

```cpp
int measurement_interval_seconds = 60;
int pm_interval_seconds = 10;           // 0 = PM sensor off
int other_sensor_interval_seconds = 10; // 0 = other sensors off
```

Only `measurement_interval_seconds` is used at runtime. It drives a single
timer in the orchestrator that calls `SensorProducer::request_measurement()`,
which invokes `SensorManager::start_measures(iterations)` — a blocking call
that reads every non-null sensor.

`pm_interval_seconds` and `other_sensor_interval_seconds` are plumbed through
NVS load/save (`go_settings.cpp`) and the UI (`go_ui.cpp`) but no operational
code reads them. They are dead settings.

## Files

| File | Change |
|---|---|
| `components/airgradient-sensors/services/sensor_manager.h` | Add `SensorGroup` enum + operators, update `start_measures` signature |
| `components/airgradient-sensors/services/sensor_manager.cpp` | Conditional accumulation by group, skip delay when `iterations == 1` |
| `products/go/main/go_settings.h` | Remove `measurement_interval_seconds` |
| `products/go/main/go_settings.cpp` | Remove load/save/validate for NVS key `"mis"` |
| `products/go/main/go_sensor_producer.h` | Update `request_measurement` signature |
| `products/go/main/go_sensor_producer.cpp` | Encode/decode group in task notification, pass group to SensorManager |
| `products/go/main/go_orchestrator.h` | Two timer timestamps, cached measures, remove `_last_measurement_ms` and `compute_iterations()` |
| `products/go/main/go_orchestrator.cpp` | Two-timer logic, updated timeout/sleep calculations, data merging |
| `products/go/main/main.cpp` | Fast-path: `start_measures(1, SensorGroup::All)`, updated sleep duration |
| `products/go/specs/orchestrator.md` | Update timer, sleep, and iteration sections to reflect new design |
| `components/airgradient-sensors/tests/sensor_manager.tests.cpp` | Tests for `SensorGroup` filtering and delay skip |
| `products/go/tests/go_orchestrator.tests.cpp` | Tests for two independent timers, combined fire, disabled groups, data merging, sleep duration |

## Design Decisions

### Orchestrator Owns Scheduling

The orchestrator already has the event loop, timer infrastructure, sleep
calculation, and settings. Moving scheduling into `SensorProducer` would
trade orchestrator complexity for producer complexity without a net gain.
The orchestrator still needs timing information for sleep duration and
immediate measurements on init/unlock, so it would need to know the
intervals regardless.

### Single SensorProducer Task, Sequential Execution

All AGo sensors share a single I2C bus. Sequential execution avoids bus
contention and does not require a mutex. A second task would double the
stack memory (~4 KB) with no benefit given that measurement duration is
short (~10–50 ms with 1 iteration).

### Single Event Type With Invalid Sentinels

`SensorDataReady` already carries a `MeasuresAGo` struct. Unmeasured fields
carry invalid sentinels (the default initialization). Consumers already use
`is_*_valid()` checks for display and storage. No new event types are
needed.

### Always 1 Iteration

Sensors on the AGo (SPS30, STCC4, SGP41, DPS368) perform internal
averaging and filtering. A single firmware read returns the sensor's
already-processed output. Multi-iteration firmware averaging is
belt-and-suspenders that adds no meaningful data quality.

More critically, the current formula `iterations = interval_ms / 2000`
makes each measurement's duration equal to the reporting interval. With two
timers sharing a single task, both groups would each consume 100% of the
task time — making it impossible to schedule both. Using 1 iteration per
measurement (~10–50 ms) leaves ample headroom at any interval >= 1 s.

### Skip Delay When iterations == 1

`SensorManager::start_measures()` pads each iteration to
`CONFIG_AVERAGING_ITERATION_INTERVAL_MS` (2000 ms). With 1 iteration, this
delay is dead time — the sensor reads are already complete. Skipping the
delay when `iterations == 1` makes the measurement return as fast as the
I2C reads complete. The `CONFIG_AVERAGING_ITERATION_INTERVAL_MS` constant
is unchanged; multi-iteration averaging remains available for other
products.

### Combined Request on Simultaneous Deadlines

When both PM and other-sensor timers fire at the same time, the
orchestrator sends a single `SensorGroup::All` request. This avoids the
task-notification overwrite race (only one `uint32_t` value can be pending)
and is more efficient than two sequential measurements.

### Group-Based Overwrite, Not Validity-Based Merge

When a `SensorDataReady` event arrives, the orchestrator must decide which
fields in `_cached_measures` to overwrite. Two approaches were considered:

1. **Validity-based merge:** Only overwrite fields that came back valid.
   Retain old cached values for sentinel fields. Problem: if a sensor
   starts failing after previously succeeding, the stale value persists
   indefinitely with no indication to the user.

2. **Group-based overwrite:** Always overwrite all fields belonging to the
   requested group, regardless of whether the new values are valid or
   sentinel. If the PM group was requested and PM reads fail, the PM
   fields go to sentinel and the display shows dashes — correctly
   reflecting the current sensor state.

Option 2 is chosen. The orchestrator tracks the last-requested
`SensorGroup` and uses it to decide which fields to overwrite on the next
`SensorDataReady`. This ensures sensor failures are immediately visible
to the user rather than masked by stale cached data.

## SensorGroup

A bitmask enum in `sensor_manager.h` that controls which sensors
`start_measures()` polls:

```cpp
enum class SensorGroup : uint8_t {
    None  = 0x00,
    PM    = 0x01,
    Other = 0x02,
    All   = 0x03,
};

inline SensorGroup operator|(SensorGroup a, SensorGroup b) {
    return static_cast<SensorGroup>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool has_group(SensorGroup mask, SensorGroup flag) {
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0;
}
```

### Group Membership

| Group | Sensors Polled | Accumulate Calls |
|---|---|---|
| `PM` | `pms_a`, `pms_b` | `_accumulate_pm_sensor()` x2 |
| `Other` | `temp_hum`, `co2`, `tvoc_nox`, `o3_no2`, `pressure` | `_accumulate_temp_hum_a_fallback()`, `_accumulate_co2()`, `_accumulate_tvoc_nox()`, `_accumulate_o3_no2()`, `_accumulate_pressure()` |
| `All` | All of the above | All accumulate calls (current behavior) |

### Temp/Hum Fallback

Temp/hum is always in the `Other` group, even when the resolved source is
`TempHumSource::PM_A`. In that case, `_accumulate_temp_hum_a_fallback()`
calls `pms_a->temp_hum_data()` which returns cached data from the PM
sensor's last read — no new PM measurement is triggered.

On AGo the fallback priority is `CO2 > PRESSURE`, so `PM_A` is never the
temp/hum source. This is a non-issue on AGo but documented for
completeness.

### PM Sensor B Temp/Hum

`_accumulate_pm_sensor()` for sensor B conditionally accumulates
`temp_hum_b` when no dedicated sensor exists and sensor B supports
temp/hum. This only executes during `PM` group cycles. On AGo, `pms_b` is
`nullptr`, so this path is never reached.

## SensorManager Changes

### Updated Signature

```cpp
Measures start_measures(int iterations, SensorGroup groups = SensorGroup::All);
```

Default parameter preserves backward compatibility for other products that
call `start_measures(n)` without a group argument.

### Conditional Accumulation

Inside the iteration loop, accumulate calls are gated on the group mask:

```
for i in 0..iterations:
    start_time = RTOS::get_time_ms()

    if has_group(groups, Other):
        _accumulate_temp_hum_a_fallback(...)
        _accumulate_co2(...)
        _accumulate_tvoc_nox(...)
        _accumulate_o3_no2(...)
        _accumulate_pressure(...)

    if has_group(groups, PM):
        _accumulate_pm_sensor(pms_a, ...)
        _accumulate_pm_sensor(pms_b, ...)

    // Skip delay for single-iteration measurements
    if iterations > 1:
        elapsed = RTOS::get_time_ms() - start_time
        if elapsed < CONFIG_AVERAGING_ITERATION_INTERVAL_MS:
            RTOS::delay_ms(CONFIG_AVERAGING_ITERATION_INTERVAL_MS - elapsed)
```

Skipped groups leave their counters at zero. The existing `_calculate_*_average()`
functions return invalid sentinels when the counter is zero. No special
handling needed.

Pre-loop setup (`_resolve_temp_hum_a_source()`, PM-B capability caching) is
harmless to run unconditionally and avoids conditional complexity.

## SensorProducer Changes

### Updated Signature

```cpp
void request_measurement(uint8_t iterations, SensorGroup groups);
```

### Task Notification Encoding

Pack both values into the `uint32_t` task notification:

```cpp
void SensorProducer::request_measurement(uint8_t iterations, SensorGroup groups) {
    if (_task_handle != nullptr) {
        uint32_t value = (static_cast<uint32_t>(groups) << 8) | iterations;
        RTOS::task_notify_send(_task_handle, value);
    }
}
```

Decode in `run()`:

```
raw = task_notify_wait()
iterations = raw & 0xFF
groups     = (raw >> 8) & 0xFF

if iterations == 0: iterations = 1
if groups == None:   groups = All

measures = _manager.start_measures(iterations, groups)
```

The rest of the function (mapping `Measures` to `MeasuresAGo`, posting
`SensorDataReady`) is unchanged. Unmeasured fields carry invalid sentinels
through to the event.

## GoSettings Changes

### Remove measurement_interval_seconds

**`go_settings.h`:** Remove field:

```cpp
// Remove:
int measurement_interval_seconds = 60;

// Keep (already exist):
int pm_interval_seconds = 10;           // 0 = PM sensor off
int other_sensor_interval_seconds = 10; // 0 = other sensors off
```

**`go_settings.cpp`:**
- Remove `is_measurement_interval_valid()`
- Remove NVS load for key `"mis"` from `load_go_settings()`
- Remove NVS save for key `"mis"` from `save_go_settings()`
- Remove `is_measurement_interval_valid()` call from save validation
- `is_sensor_interval_valid()` already validates pm/other intervals

### NVS Migration

`load_go_settings()` reads keys individually with fallback to defaults.
An orphaned `"mis"` key in NVS from old firmware is simply never read. No
explicit migration logic needed.

## Orchestrator Changes

### Timer State

Replace single timer with two independent timers:

```cpp
// Remove:
uint32_t _last_measurement_ms = 0;

// Add:
uint32_t _last_pm_measurement_ms = 0;
uint32_t _last_other_measurement_ms = 0;

// Add — tracks which group the last request targeted:
SensorGroup _last_requested_group = SensorGroup::None;

// Add — merged results from partial measurements:
MeasuresAGo _cached_measures{};
```

Remove `compute_iterations()` — iterations are always 1.

### check_timers()

```
now = RTOS::get_time_ms()

pm_enabled    = _settings.pm_interval_seconds > 0
other_enabled = _settings.other_sensor_interval_seconds > 0
pm_interval   = _settings.pm_interval_seconds * 1000
other_interval = _settings.other_sensor_interval_seconds * 1000

pm_due    = pm_enabled    and (now - _last_pm_measurement_ms)    >= pm_interval
other_due = other_enabled and (now - _last_other_measurement_ms) >= other_interval

groups = SensorGroup::None
if pm_due:    groups = groups | SensorGroup::PM
if other_due: groups = groups | SensorGroup::Other

if groups != SensorGroup::None:
    _svc.sensor_producer.request_measurement(1, groups)
    _last_requested_group = groups
    if pm_due:    _last_pm_measurement_ms = now
    if other_due: _last_other_measurement_ms = now
```

At most one `request_measurement()` call per `check_timers()` invocation.
When both deadlines coincide, a single `SensorGroup::All` request is sent.

### on_measurement_timer()

Removed. Its logic is inlined into `check_timers()`.

### compute_queue_timeout_ms()

Replace single measurement deadline with two. Exclude disabled groups
(interval == 0) from the minimum:

```
now = RTOS::get_time_ms()
next = UINT32_MAX

// PM timer
if _settings.pm_interval_seconds > 0:
    pm_deadline = _last_pm_measurement_ms + (_settings.pm_interval_seconds * 1000)
    remaining = pm_deadline - now
    next = min(next, remaining)

// Other-sensor timer
if _settings.other_sensor_interval_seconds > 0:
    other_deadline = _last_other_measurement_ms + (_settings.other_sensor_interval_seconds * 1000)
    remaining = other_deadline - now
    next = min(next, remaining)

// BMS
bms_deadline = _last_bms_poll_ms + BMS_POLL_INTERVAL_MS
next = min(next, bms_deadline - now)

// Inactivity (only when unlocked and timeout > 0)
if _lock_state == Unlocked and _settings.inactivity_timeout_seconds > 0:
    inact_deadline = _last_input_ms + (_settings.inactivity_timeout_seconds * 1000)
    next = min(next, inact_deadline - now)

// Clamp overdue deadlines
if next > MAX_REASONABLE_TIMEOUT_MS:
    next = 0

return next
```

### on_sensor_data() — Group-Based Overwrite

```
on_sensor_data(data):
    // Overwrite all fields belonging to the requested group, regardless
    // of whether the new values are valid or sentinel.  This ensures
    // sensor failures are immediately visible (display shows "-") rather
    // than masked by stale cached data.

    if has_group(_last_requested_group, SensorGroup::PM):
        _cached_measures.pm_a = data.pm_a

    if has_group(_last_requested_group, SensorGroup::Other):
        _cached_measures.co2 = data.co2
        _cached_measures.temp_hum_a = data.temp_hum_a
        _cached_measures.tvoc_nox = data.tvoc_nox
        _cached_measures.pressure = data.pressure

    _cached_measures.power = data.power    // always update

    _first_measurement_done = true

    _svc.storage.cache_measurement(_cached_measures)

    if _tracking_active:
        RoutePoint point = { time(nullptr), _latest_gps, _cached_measures }
        _svc.storage.append_route_point(point)

    update_display()
```

The overwrite is keyed on `_last_requested_group`, not on whether the
incoming field values are valid. This means:

- **PM-only cycle:** PM fields are overwritten (valid or sentinel). Other
  fields (CO2, temp, TVOC, pressure) are untouched — they retain their
  values from the last Other cycle.
- **Other-only cycle:** Other fields are overwritten. PM fields are
  untouched — they retain their values from the last PM cycle.
- **Combined (All) cycle:** All fields are overwritten.
- **Sensor failure:** If PM reads fail at t=20s (after succeeding at
  t=10s), the PM fields go to sentinel and the display shows dashes. The
  stale t=10s value is not retained.

This relies on a single assumption: the next `SensorDataReady` event
corresponds to `_last_requested_group`. This holds because there is a
single SensorProducer task processing requests sequentially, and
`check_timers()` sends at most one request per invocation.

### init() and unlock()

Change immediate measurement requests to include `SensorGroup::All`:

```
// In init():
_svc.sensor_producer.request_measurement(1, SensorGroup::All)
_last_requested_group = SensorGroup::All
_last_pm_measurement_ms = now
_last_other_measurement_ms = now

// In unlock():
_svc.sensor_producer.request_measurement(1, SensorGroup::All)
_last_requested_group = SensorGroup::All
```

### compute_sleep_duration_ms()

Replace `measurement_interval_seconds` with the minimum enabled interval:

```
compute_sleep_duration_ms():
    duration = UINT32_MAX

    if _settings.pm_interval_seconds > 0:
        duration = min(duration, _settings.pm_interval_seconds * 1000)

    if _settings.other_sensor_interval_seconds > 0:
        duration = min(duration, _settings.other_sensor_interval_seconds * 1000)

    if _settings.display_refresh_interval_seconds > 0:
        duration = min(duration, _settings.display_refresh_interval_seconds * 1000)

    // Fallback if everything is disabled
    if duration == UINT32_MAX:
        duration = 60000

    return duration
```

### apply_settings_change()

No additional logic needed. When the user changes PM or other-sensor
interval in the UI, `apply_to_settings()` writes the new value to
`_settings`. The next `check_timers()` call uses the new interval. Timer
timestamps are not reset — the new interval applies from the last
measurement time.

## Fast-Path Boot Changes

### main.cpp — run_fast_path()

Update one-shot measurement:

```
// Was:
measures = sensor_manager.start_measures(1)

// Now:
measures = sensor_manager.start_measures(1, SensorGroup::All)
```

`SensorGroup::All` is the default parameter, so this is technically a
no-op change. Made explicit for clarity.

### compute_fast_path_sleep_duration()

Same change as `Orchestrator::compute_sleep_duration_ms()`:

```
compute_fast_path_sleep_duration(settings):
    duration = UINT32_MAX

    if settings.pm_interval_seconds > 0:
        duration = min(duration, settings.pm_interval_seconds * 1000)

    if settings.other_sensor_interval_seconds > 0:
        duration = min(duration, settings.other_sensor_interval_seconds * 1000)

    if settings.display_refresh_interval_seconds > 0:
        duration = min(duration, settings.display_refresh_interval_seconds * 1000)

    if duration == UINT32_MAX:
        duration = 60000

    return duration
```

## UI Changes

None. PM interval and other-sensor interval are already fully wired in
the settings UI (`go_ui.cpp`). The `measurement_interval_seconds` setting
had no UI representation.

## Edge Cases

| Case | Behavior |
|---|---|
| PM=0 (off), Other=60s | Only other-sensor timer runs. PM fields remain at invalid sentinels. |
| PM=10s, Other=0 (off) | Only PM timer runs. Other fields remain at invalid sentinels. Temp/hum unavailable on AGo (source is CO2, an Other-group sensor). |
| PM=0, Other=0 | No measurements scheduled. `_cached_measures` stays at invalid sentinels. Sleep uses display interval or 60s fallback. |
| Both due simultaneously | Single `SensorGroup::All` request. Both timestamps updated. |
| Settings change mid-cycle | New intervals take effect on next `check_timers()`. Timestamps are not reset. |
| Measurement in progress when timer fires | Task notification is pending. SensorProducer picks it up after current measurement completes. At most one notification pending (guaranteed by single combined request per `check_timers()`). |
| 1s PM, 1s Other | Both always fire together -> combined `All` request every 1s. Measurement takes ~50 ms, 950 ms idle. |
| Unlock while measurement in progress | `request_measurement(1, All)` sends notification. If task is busy, it queues as pending. Previous in-flight result posts `SensorDataReady`, then the unlock request executes. Two rapid data events — harmless, group-based overwrite handles it. |
| PM sensor starts failing | PM fields go to sentinel on next PM cycle. Display shows dashes for PM. Other fields are untouched — retain values from their last cycle. |
| Other sensors start failing | Other fields go to sentinel on next Other cycle. Display shows dashes. PM fields retain values from last PM cycle. |
| All sensors fail | All fields go to sentinel. Display shows all dashes. Correctly reflects hardware state. |

## Testability

### SensorManager Tests

| Scenario | Verify |
|---|---|
| `start_measures(1, SensorGroup::PM)` | Only PM fields populated, all other fields are invalid sentinels |
| `start_measures(1, SensorGroup::Other)` | Only other-sensor fields populated, PM fields are invalid sentinels |
| `start_measures(1, SensorGroup::All)` | All fields populated (same as current behavior) |
| `start_measures(1, ...)` | No `RTOS::delay_ms` call (delay skipped for 1 iteration) |
| `start_measures(5, SensorGroup::All)` | Delay still applies for multi-iteration (backward compatibility) |
| Null sensors in active group | Null-check returns early, fields stay at invalid sentinels |

### Orchestrator Tests

| Scenario | Verify |
|---|---|
| PM interval elapses | `request_measurement(1, SensorGroup::PM)` called, PM timestamp updated |
| Other interval elapses | `request_measurement(1, SensorGroup::Other)` called, other timestamp updated |
| Both intervals elapse | Single `request_measurement(1, SensorGroup::All)` call, both timestamps updated |
| PM disabled (interval=0) | No PM timer fires, only other-sensor timer active |
| Other disabled (interval=0) | No other timer fires, only PM timer active |
| Both disabled | No measurements scheduled, no `request_measurement` calls |
| `on_sensor_data` with PM-only result | `_cached_measures.pm_a` overwritten, other fields unchanged |
| `on_sensor_data` with Other-only result | Other fields overwritten, `_cached_measures.pm_a` unchanged |
| `on_sensor_data` PM requested but read fails | `_cached_measures.pm_a` set to sentinel (not stale cached value), other fields unchanged |
| `on_sensor_data` Other requested but read fails | Other fields set to sentinel, PM field unchanged |
| `compute_queue_timeout_ms` | Returns minimum of enabled timer deadlines |
| `compute_sleep_duration_ms` | Returns minimum of enabled intervals and display refresh |
| `compute_sleep_duration_ms` all disabled | Returns 60000 ms fallback |
| `init()` | Requests `SensorGroup::All`, sets both timestamps |
| `unlock()` | Requests `SensorGroup::All` |
| Settings change | New interval used on next `check_timers()` without timestamp reset |

## What Is Not In This Spec

- No change to `CONFIG_AVERAGING_ITERATION_INTERVAL_MS` value
- No PM power management (load switch on/off per measurement cycle)
- No change to other products (default parameter preserves existing behavior)
- No new NVS keys (PM and other interval keys already exist)
