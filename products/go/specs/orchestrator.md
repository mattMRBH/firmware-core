# Orchestrator — Implementation Spec

Central event loop for AirGradient Go. Consumes events from the shared queue,
manages application state (operating mode, behavior, lock/unlock), coordinates
all product services, handles timer-based periodic tasks, and controls the
sleep cycle.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_orchestrator.h` | `Orchestrator` class declaration |
| `products/go/main/go_orchestrator.cpp` | Event loop, dispatch, state transitions, timer logic |

Add `go_orchestrator.cpp` to the `SRCS` list in
`products/go/main/CMakeLists.txt`.

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `SensorProducer` | product (`go_sensor_producer.h`) | Request measurement cycles |
| `GpsService` | product (`go_gps.h`) | Read latest GPS fix |
| `InputService` | product (`go_input.h`) | Started/stopped by orchestrator; posts `InputPress` events |
| `DisplayService` | product (`go_display.h`) | Render display frames |
| `StorageService` | product (`go_storage.h`) | Cache measurements, persist route data |
| `PowerService` | product (`go_power.h`) | BMS polling, sleep entry, RTC state, shutdown |
| `UIManager` | product (`go_ui.h`) | Screen navigation, input dispatch, display value building |
| `ConfigStore` | `airgradient-config` | Load/save `GoSettings` to NVS |
| `GoSettings` | product (`go_settings.h`) | Product configuration |
| `Event`, `EventType` | product (`go_events.h`) | Event queue types |
| `RtcAppState` | product (`go_types.h`) | State persisted across deep sleep |
| RTOS | `airgradient-common` | Queue receive, time query, delay |

## Class Design

```cpp
#pragma once

#include "config_store.h"
#include "go_display.h"
#include "go_events.h"
#include "go_gps.h"
#include "go_input.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_types.h"
#include "go_ui.h"
#include "rtos.h"

#include <cstdint>

class Orchestrator {
public:
  struct Services {
    SensorProducer &sensor_producer;
    GpsService &gps_service;
    InputService &input_service;
    DisplayService &display_service;
    StorageService &storage_service;
    PowerService &power_service;
    UIManager &ui_manager;
  };

  Orchestrator(RtosQueueHandle event_queue, const Services &services,
               GoSettings settings, ConfigStore &config_store);

  /// Set initial state from boot context and perform first-boot actions.
  /// Call once before run().
  void init(WakeCause cause);

  /// Main event loop. Does not return.
  void run();

private:
  RtosQueueHandle _event_queue;
  Services _svc;
  GoSettings _settings;
  ConfigStore &_config_store;

  // --- Application state ---
  OperatingMode _mode = OperatingMode::Offline;
  Behavior _behavior = Behavior::Idle;
  LockState _lock_state = LockState::Locked;
  bool _gps_enabled = true;
  bool _tracking_active = false;
  uint32_t _tracking_session_id = 0;

  // --- Cached data ---
  MeasuresAGo _latest_measures{};
  GpsData _latest_gps{};
  PowerSnapshot _latest_power{};

  // --- Timer tracking (millisecond timestamps) ---
  uint32_t _last_measurement_ms = 0;
  uint32_t _last_bms_poll_ms = 0;
  uint32_t _last_input_ms = 0; ///< reset on every input; drives inactivity
  bool _first_measurement_done = false;

  // --- Constants ---
  static constexpr uint32_t BMS_POLL_INTERVAL_MS = 5000;

  // --- Event dispatch ---
  void dispatch(const Event &event);

  // --- Event handlers ---
  void on_sensor_data(const MeasuresAGo &data);
  void on_gps_fix(const GpsData &data);
  void on_input(const InputEventData &input);

  // --- State transitions ---
  void lock();
  void unlock();
  void start_tracking();
  void stop_tracking();
  void change_mode(OperatingMode new_mode);
  void apply_settings_change();
  void clear_data();
  void save_tag(uint8_t tag_index);
  void shutdown();

  // --- Timer management ---
  uint32_t compute_queue_timeout_ms() const;
  void check_timers();
  void on_measurement_timer();
  void on_bms_timer();
  void on_inactivity_timeout();

  // --- Display ---
  void update_display();
  BuildContext build_context() const;

  // --- Sleep ---
  void try_enter_sleep();
  void prepare_for_sleep();
  uint32_t compute_sleep_duration_ms() const;

  // --- Helpers ---
  bool is_gps_active() const;
  uint8_t compute_iterations() const;
  uint32_t generate_session_id();
  RtcAppState snapshot_state() const;
};
```

## App State

The orchestrator owns the authoritative application state:

| Field | Type | Meaning |
|---|---|---|
| `_mode` | `OperatingMode` | Portable / Stationary / Offline |
| `_behavior` | `Behavior` | Tracking / Idle / Shutdown |
| `_lock_state` | `LockState` | Locked / Unlocked |
| `_gps_enabled` | `bool` | Whether GPS data is used (derived from `GpsMode` setting) |
| `_tracking_active` | `bool` | True while a route is being logged |
| `_tracking_session_id` | `uint32_t` | 5-digit session ID; 0 = no active session |

On fresh boot, defaults from `RtcAppState` are used (Offline, Idle, Locked,
GPS on, not tracking). On button wake, state is restored from RTC memory via
`load_rtc_app_state()`.

## Event Loop

The `run()` method is an infinite loop using queue-timeout polling for timers:

```
run():
    while true:
        // Sleep check: enter sleep when locked and ready
        if _lock_state == Locked and _first_measurement_done:
            try_enter_sleep()
            // Returns only for light sleep wake or if sleep was not entered

        timeout = compute_queue_timeout_ms()
        event = Event{}
        got_event = RTOS::queue_receive(_event_queue, &event, timeout)

        if got_event:
            dispatch(event)

        check_timers()
```

The loop alternates between processing events and checking timer deadlines.
Sleep entry is attempted at the top of each iteration when locked, ensuring
the device sleeps as soon as possible after completing pending work.

`_first_measurement_done` prevents sleeping before the initial measurement
completes on boot. The flag is set in `on_sensor_data()` when the first
`SensorDataReady` event is processed.

## Timer Management

No dedicated timer tasks or callbacks. The orchestrator tracks deadlines
using `RTOS::get_time_ms()` and computes the queue-receive timeout from the
nearest deadline.

### Active Timers

| Timer | Interval | Active When |
|---|---|---|
| Measurement | `_settings.measurement_interval_seconds × 1000` | Always |
| BMS poll + watchdog | `BMS_POLL_INTERVAL_MS` (5000 ms) | Always |
| Inactivity | `_settings.inactivity_timeout_seconds × 1000` | Unlocked only |

### compute_queue_timeout_ms()

```
now = RTOS::get_time_ms()
next = UINT32_MAX

// Measurement
meas_deadline = _last_measurement_ms + (_settings.measurement_interval_seconds * 1000)
remaining = meas_deadline - now       // unsigned subtraction handles wrap
next = min(next, remaining)

// BMS
bms_deadline = _last_bms_poll_ms + BMS_POLL_INTERVAL_MS
next = min(next, bms_deadline - now)

// Inactivity (only when unlocked and timeout > 0)
if _lock_state == Unlocked and _settings.inactivity_timeout_seconds > 0:
    inact_deadline = _last_input_ms + (_settings.inactivity_timeout_seconds * 1000)
    next = min(next, inact_deadline - now)

// If any deadline already passed, the unsigned subtraction yields a large
// number — clamp to 0 so check_timers() fires immediately.
if next > some_reasonable_max (e.g. 3600000):
    next = 0

return next
```

### check_timers()

```
now = RTOS::get_time_ms()

if (now - _last_measurement_ms) >= (_settings.measurement_interval_seconds * 1000):
    on_measurement_timer()

if (now - _last_bms_poll_ms) >= BMS_POLL_INTERVAL_MS:
    on_bms_timer()

if _lock_state == Unlocked
   and _settings.inactivity_timeout_seconds > 0
   and (now - _last_input_ms) >= (_settings.inactivity_timeout_seconds * 1000):
    on_inactivity_timeout()
```

### on_measurement_timer()

```
iterations = compute_iterations()
_svc.sensor_producer.request_measurement(iterations)
_last_measurement_ms = RTOS::get_time_ms()
```

### on_bms_timer()

```
_latest_power = _svc.power.poll_bms()
_svc.power.reset_watchdog()
_last_bms_poll_ms = RTOS::get_time_ms()
```

### on_inactivity_timeout()

```
lock()
```

## Event Dispatch

```
dispatch(event):
    switch event.type:
        SensorDataReady    -> on_sensor_data(event.sensor_data)
        GpsFixUpdate       -> on_gps_fix(event.gps_data)
        InputPress         -> on_input(event.input)

        // UI action events (reserved for future programmatic triggers,
        // e.g. BLE commands)
        UserStartTracking  -> start_tracking()
        UserStopTracking   -> stop_tracking()
        UserChangeMode     -> change_mode(event.mode_change)
        UserToggleGps      -> _gps_enabled = event.gps_enabled
        SettingsChanged    -> apply_settings_change()
        ClearData          -> clear_data()
        SaveTag            -> save_tag(event.tag_index)

        // System events
        InactivityTimeout  -> on_inactivity_timeout()
        MeasurementTimer   -> on_measurement_timer()
        WakeFromSleep      -> // unused; wake handled in init()
```

### on_sensor_data(data)

```
_latest_measures = data
_first_measurement_done = true

_svc.storage.cache_measurement(data)

if _tracking_active:
    RoutePoint point = { time(nullptr), _latest_gps, data }
    _svc.storage.append_route_point(point)

update_display()
```

### on_gps_fix(data)

```
if not is_gps_active():
    return                  // GPS disabled in settings; ignore

_latest_gps = data
```

GPS data is consumed passively. It is used when building route points (in
`on_sensor_data`) and when building display values (`update_display`).

### on_input(input)

```
_last_input_ms = RTOS::get_time_ms()     // reset inactivity timer

// --- Shutdown: long press on power button (any lock state) ---
if input.source == ButtonPower and input.type == LongPress:
    shutdown()
    return

// --- Factory reset stub: long press on boot button ---
if input.source == ButtonBoot and input.type == LongPress:
    // TODO: factory reset implementation
    return

// --- Lock toggle: power button short press ---
if input.source == ButtonPower and input.type == ShortPress:
    if _lock_state == Locked:
        unlock()
    else:
        lock()
    return

// --- Locked: ignore all remaining inputs ---
if _lock_state == Locked:
    return

// --- Unlocked: forward to UI Manager ---
UIActionResult result = _svc.ui_manager.handle_input(input.source, input.type)

switch result.action:
    StartTracking   -> start_tracking()
    StopTracking    -> stop_tracking()
    ChangeMode      -> change_mode(result.new_mode)
    SettingsChanged -> apply_settings_change()
    ClearData       -> clear_data()
    SaveTag         -> save_tag(result.tag_index)
    None            -> pass   // pure UI navigation

update_display()
```

## State Transitions

### lock()

```
_lock_state = LockState::Locked
_svc.ui_manager.reset_to_home()
update_display()
// Sleep entry is handled by the main loop on its next iteration
```

### unlock()

```
_lock_state = LockState::Unlocked
_last_input_ms = RTOS::get_time_ms()

// Request a quick measurement so the user sees fresh data
_svc.sensor_producer.request_measurement(1)

update_display()
```

### start_tracking()

```
if _tracking_active:
    return                          // already tracking

_tracking_session_id = generate_session_id()
_tracking_active = true
_behavior = Behavior::Tracking

_svc.storage.start_route(_tracking_session_id)
_svc.ui_manager.show_snackbar("Tracking started")
update_display()
```

### stop_tracking()

```
if not _tracking_active:
    return

_svc.storage.end_route()
_tracking_active = false
_tracking_session_id = 0
_behavior = Behavior::Idle

_svc.ui_manager.show_snackbar("Tracking stopped")
update_display()
```

### change_mode(new_mode)

Stub — operating mode logic (BLE, WiFi, HTTP server) is not yet
implemented. Sets the mode field and shows a confirmation snackbar.

```
_mode = new_mode
_svc.ui_manager.show_snackbar("Mode changed")
update_display()

// Future: enable/disable BLE, WiFi, HTTP server based on mode
```

### apply_settings_change()

Called when the UI Manager signals that the user changed a setting.

```
_svc.ui_manager.apply_to_settings(_settings)
save_go_settings(_config_store, _settings)

// Propagate runtime changes to services
_svc.gps_service.set_posting_interval_ms(_settings.gps_interval_seconds * 1000)
_gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff)
```

**Required addition to UIManager:** `apply_to_settings(GoSettings &)` converts
internal option indices back to `GoSettings` field values. This is the reverse
of the existing `sync_settings(const GoSettings &)`.

### clear_data()

```
if _tracking_active:
    stop_tracking()

// Clear persistent route data and temporary cache
// Exact StorageService API TBD
_svc.ui_manager.show_snackbar("Data cleared")
update_display()
```

### save_tag(tag_index, tag_label)

```
// Persist the tag association with the current route point
// Exact mechanism TBD (StorageService or metadata file)
_svc.ui_manager.show_snackbar("Tag '<tag_label>' saved")
update_display()
```

### shutdown()

```
if _tracking_active:
    stop_tracking()

_svc.storage.backup_cache()
_svc.ui_manager.set_screen(Screen::Shutdown)
update_display()

// Allow display to finish e-paper refresh
RTOS::delay_ms(500)

_svc.power.shutdown()       // BMS QoN — does not return
```

## Display Update

The orchestrator builds a `BuildContext` from its cached state and asks
`UIManager` to produce a `DisplayValues` snapshot:

```
update_display():
    _svc.ui_manager.clear_expired_snackbar(RTOS::get_time_ms())
    BuildContext ctx = build_context()
    DisplayValues values = _svc.ui_manager.build_values(ctx)
    _svc.display_service.update(values)
```

`build_context()` aggregates the orchestrator's cached sensor data, GPS
clock, battery status, lock state, tracking state, and settings flags into
a `BuildContext` struct.

**Chart data:** `build_context()` calls
`_svc.storage.read_cache(cache_buf, UI_CHART_BUF_SIZE)` to fill a local
`MeasuresAGo` array, then passes the pointer and count via `BuildContext`.

**Required addition to StorageService:**
`uint16_t read_cache(MeasuresAGo *out, uint16_t max_count) const` — copies
all cached entries (oldest first) into a caller-provided buffer and returns
the number of entries written. This complements the existing
`read_cached_field()` method.

## Sleep Entry

```
try_enter_sleep():
    sleep_type = _svc.power.evaluate_sleep(_settings, _lock_state)

    if sleep_type == SleepType::None:
        return              // should not happen when locked, but guard

    prepare_for_sleep()

    if sleep_type == Deep:
        _svc.power.enter_sleep(Deep, compute_sleep_duration_ms())
        // never returns — CPU reboots on wake

    if sleep_type == Light:
        cause = _svc.power.enter_sleep(Light, compute_sleep_duration_ms())
        // returns here on wake
        restart_services()
        if cause == Button:
            unlock()
        else:
            // Timer wake while locked: run one measurement inline,
            // then the main loop will re-enter try_enter_sleep()
            on_measurement_timer()
```

### prepare_for_sleep()

```
// Ensure pending display refresh completes before stopping worker
_svc.display_service.update(build_display_values(), true)   // wait = true

_svc.sensor_producer.stop()
_svc.gps_service.stop()
_svc.input_service.stop()
_svc.display_service.stop()

_svc.storage.backup_cache()
_svc.power.save_state(snapshot_state())
```

### compute_sleep_duration_ms()

```
duration = _settings.measurement_interval_seconds * 1000

if _settings.display_refresh_interval_seconds > 0:
    display_ms = _settings.display_refresh_interval_seconds * 1000
    duration = min(duration, display_ms)

return duration
```

## Boot Initialization

`init(cause)` sets up the orchestrator state based on how the device booted:

```
init(cause):
    if cause == WakeCause::Button:
        state = _svc.power.load_state()
        _mode = state.mode
        _behavior = state.behavior
        _gps_enabled = state.gps_enabled
        _tracking_active = state.tracking_active
        _tracking_session_id = state.tracking_session_id
        unlock()                   // user pressed button to wake

        // Resume route file if tracking was active before sleep
        if _tracking_active:
            _svc.storage.start_route(_tracking_session_id)

    else:   // WakeCause::PowerOn (fresh boot)
        // Defaults already set by member initializers
        // (Offline, Idle, Locked, GPS on, not tracking)

    _svc.ui_manager.sync_settings(_settings)

    // Initial measurement (single iteration for fast first reading)
    _svc.sensor_producer.request_measurement(1)

    // Initial BMS poll
    _latest_power = _svc.power.poll_bms()
    _svc.power.reset_watchdog()

    // Record timer baselines
    now = RTOS::get_time_ms()
    _last_measurement_ms = now
    _last_bms_poll_ms = now
    _last_input_ms = now

    update_display()
```

## GPS Active Logic

```
is_gps_active():
    if _settings.gps_mode == GpsMode::AlwaysOff:
        return false
    if _settings.gps_mode == GpsMode::AlwaysOn:
        return true
    // GpsMode::OnWhenTracking
    return _tracking_active
```

GPS hardware is always powered on. The GPS task always runs. This method
only controls whether the orchestrator *uses* GPS data — `GpsFixUpdate`
events are ignored when this returns false.

## Iteration Count

The number of averaging iterations passed to `request_measurement()`:

```
compute_iterations():
    iters = _settings.measurement_interval_seconds * 1000
            / CONFIG_AVERAGING_ITERATION_INTERVAL_MS
    return max(1, iters)
```

`CONFIG_AVERAGING_ITERATION_INTERVAL_MS` defaults to 2000 ms (defined in
`sensor_manager.cpp`).

First measurement on boot always uses 1 iteration for a fast initial
reading.

## Session ID Generation

```
generate_session_id():
    id = load from NVS counter key
    id = id + 1
    if id > 99999 or id < 10000:
        id = 10000
    save id to NVS counter key
    return id
```

Counter wraps within the 5-digit range (10000–99999). Stored in NVS so IDs
are unique across power cycles.

## snapshot_state()

Captures the current orchestrator state into an `RtcAppState` for
persistence before deep sleep:

```
snapshot_state():
    return RtcAppState {
        .mode = _mode,
        .behavior = _behavior,
        .lock_state = _lock_state,
        .gps_enabled = _gps_enabled,
        .tracking_active = _tracking_active,
        .tracking_session_id = _tracking_session_id,
    }
```

## Design Decisions

### Queue Timeout Polling vs Dedicated Timers

Queue timeout polling was chosen over `esp_timer` or FreeRTOS software
timers because:
- No additional platform dependencies
- No callback-to-queue indirection
- Timer precision requirements are coarse (seconds)
- All timer logic is centralized and testable on host

### UI Actions via Direct Return vs Queue Events

UI action events (§5.3 in ARCHITECTURE.md) are listed in `EventType` but
the primary path is through `UIManager::handle_input()` returning
`UIActionResult`. The orchestrator handles the action inline in the same
event cycle as the originating `InputPress`. The `EventType` enum values
are reserved for future programmatic triggers (e.g., BLE commands that
start tracking).

### No Separate State Machine Class

The orchestrator manages state directly with member fields rather than a
separate FSM class. The state is simple enough (3 enums + 2 booleans +
1 counter) that a dedicated FSM would add indirection without reducing
complexity.

### Operating Mode Stubs

Portable and Stationary mode behaviors (BLE streaming, WiFi HTTP server)
are not yet implemented. `change_mode()` updates the `_mode` field and
shows a snackbar. The stub is the only code path until the respective radio
services are available.

### DisplayService Under TEST_HOST

`DisplayService` is `#ifndef TEST_HOST` guarded. For host test builds, a
stub class with matching method signatures and no-op implementations is
provided so the Orchestrator compiles without conditional compilation in its
own source.

## Testability

| Function | Testable on Host? | Notes |
|---|---|---|
| `dispatch()` / event handlers | Yes (with mocks) | Core state machine logic |
| `compute_queue_timeout_ms()` | Yes | Pure arithmetic |
| `compute_iterations()` | Yes | Pure arithmetic |
| `is_gps_active()` | Yes | Pure logic |
| `check_timers()` | Yes (with RTOS mock) | Depends on `RTOS::get_time_ms()` |
| `build_context()` | Yes | Aggregates cached state |
| State transitions | Yes | Lock/unlock, tracking start/stop |
| `try_enter_sleep()` | Partial | `evaluate_sleep` is pure; `enter_sleep` is platform |
| `update_display()` | Stub | `DisplayService` stubbed under `TEST_HOST` |

Key test scenarios:
- Input while locked: only `ButtonPower` short triggers unlock
- Input while unlocked: forwarded to `UIManager`, actions handled
- Measurement timer: `SensorProducer` receives notification
- BMS poll timer: `PowerSnapshot` updated and watchdog reset
- Inactivity timeout: transitions to Locked
- Tracking start/stop: `StorageService` route management
- GPS mode: `GpsFixUpdate` ignored when GPS inactive
- Settings change: propagated to GPS service interval
- Shutdown sequence: route closed, cache backed up, BMS QoN called
