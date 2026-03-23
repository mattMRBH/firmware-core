# Orchestrator

Central event loop for AirGradient Go. Consumes events from the shared queue,
manages application state (operating mode, behavior, lock/unlock), coordinates
all product services, handles timer-based periodic tasks, and controls the
sleep cycle.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_orchestrator.h` | `Orchestrator` class declaration, `Services` struct |
| `products/go/main/go_orchestrator.cpp` | Event loop, dispatch, state transitions, timer logic, sleep |

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

## Construction

The orchestrator is constructed in `main.cpp` after all services are
initialized. It takes ownership of a copy of `GoSettings` and holds
references to all services via the `Services` aggregate:

```cpp
Orchestrator::Services services = {
    .sensor_producer = sensor_producer,
    .gps_service     = gps_service,
    .input_service   = input_service,
    .display_service = display_service,
    .storage_service = storage,
    .power_service   = power_service,
    .ui_manager      = ui_manager,
};

Orchestrator orchestrator(event_queue, services, settings, config_store);
orchestrator.init(cause);
orchestrator.run();  // never returns
```

## Application State

The orchestrator owns the authoritative application state:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `_mode` | `OperatingMode` | `Offline` | Portable / Stationary / Offline |
| `_behavior` | `Behavior` | `Idle` | Tracking / Idle / Shutdown |
| `_lock_state` | `LockState` | `Locked` | Locked / Unlocked |
| `_gps_enabled` | `bool` | `true` | Whether GPS data is used (derived from `GpsMode` setting) |
| `_tracking_active` | `bool` | `false` | True while a route is being logged |
| `_tracking_session_id` | `uint32_t` | `0` | 5-digit session ID; 0 = no active session |

On fresh boot, defaults are used. On button wake from deep sleep, state is
restored from RTC memory via `PowerService::load_state()`.

## Event Loop

The `run()` method is an infinite loop using queue-timeout polling for timers:

1. **Sleep check** — when locked and the first measurement is done, attempt
   to enter sleep. Returns only for light sleep wake or if sleep conditions
   are not met.
2. **Queue receive** — wait for the next event with a timeout computed from
   the nearest timer deadline.
3. **Dispatch** — route the event to its handler.
4. **Check timers** — fire any timer callbacks whose deadlines have elapsed.

## Timer Management

No dedicated timer tasks or callbacks. The orchestrator tracks deadlines
using `RTOS::get_time_ms()` and computes the queue-receive timeout from
the nearest deadline.

| Timer | Interval | Active When |
|---|---|---|
| Measurement | `measurement_interval_seconds * 1000` | Always |
| BMS poll + watchdog | `BMS_POLL_INTERVAL_MS` (5000 ms) | Always |
| Inactivity | `auto_lock_seconds * 1000` | Unlocked and auto-lock > 0 |

`compute_queue_timeout_ms()` returns the minimum remaining time across all
active timers, clamped to 0 when any deadline has already passed (unsigned
subtraction wraps to a large value).

## Event Dispatch

Events are dispatched by type:

| EventType | Handler |
|---|---|
| `SensorDataReady` | `on_sensor_data()` — cache, store route point if tracking, update display |
| `GpsFixUpdate` | `on_gps_fix()` — cache GPS if `is_gps_active()` |
| `InputPress` | `on_input()` — shutdown, lock/unlock, forward to UIManager |
| `UserStartTracking` | `start_tracking()` |
| `UserStopTracking` | `stop_tracking()` |
| `UserChangeMode` | `change_mode()` |
| `UserToggleGps` | Set `_gps_enabled` |
| `SettingsChanged` | `apply_settings_change()` |
| `ClearData` | `clear_data()` |
| `SaveTag` | `save_tag()` |
| `InactivityTimeout` | `on_inactivity_timeout()` → `lock()` |
| `MeasurementTimer` | `on_measurement_timer()` |
| `WakeFromSleep` | No-op (handled in `init()`) |

## Input Handling

`on_input()` processes input events with priority:

1. **Long press ButtonPower** — `shutdown()` (any lock state)
2. **Long press ButtonBoot** — factory reset (stub)
3. **Short press ButtonPower** — toggle lock/unlock
4. **Locked** — ignore all remaining inputs
5. **Unlocked** — forward to `UIManager::handle_input()`, then handle the
   returned `UIActionResult` (start/stop tracking, change mode, etc.)

## State Transitions

### lock()

Sets `LockState::Locked`, resets UI to home screen, updates display. Sleep
eligibility is evaluated on the next main loop iteration.

### unlock()

Sets `LockState::Unlocked`, resets the inactivity timer, requests a quick
single-iteration measurement, and updates the display.

### start_tracking() / stop_tracking()

Manages route lifecycle through `StorageService`. Generates a 5-digit
session ID (NVS counter, range 10000–99999), opens/closes the route file,
and toggles `_behavior` between `Tracking` and `Idle`.

### change_mode()

Stub — sets `_mode` and shows a snackbar. BLE/WiFi/HTTP server logic is
deferred until those radio services are implemented.

### apply_settings_change()

Called when the UI signals a setting was changed. Calls
`UIManager::apply_to_settings()` to convert internal option indices back to
`GoSettings` fields, persists to NVS via `save_go_settings()`, and
propagates runtime changes (GPS posting interval, GPS enabled flag).

### shutdown()

Stops tracking if active, backs up the cache, shows the shutdown screen,
waits for the display refresh (500 ms), then calls
`PowerService::shutdown()` (BMS ship mode — does not return).

## Display Update

`update_display()` builds a `BuildContext` from cached state and asks the
UIManager to produce a `DisplayValues` snapshot:

1. Clear expired snackbar
2. `build_context()` — convert cached `MeasuresAGo` to `Measures`, read
   chart cache, extract GPS clock, battery info, and status flags
3. `UIManager::build_values(ctx)` — produce `DisplayValues`
4. `DisplayService::update(values)` — non-blocking render submission

The `BuildContext` requires a `const Measures &` reference. The orchestrator
maintains a `mutable Measures _display_measures` member that is populated
from the cached `MeasuresAGo` each time `build_context()` is called.

## Sleep Cycle

### Entry

`try_enter_sleep()` is called at the top of each loop iteration when the
device is locked and the first measurement is complete:

1. `PowerService::evaluate_sleep()` determines the sleep type (None, Light,
   Deep) based on settings and lock state.
2. `prepare_for_sleep()` — wait for display refresh, stop all task-based
   services, backup cache, save RTC state.
3. **Deep sleep** — `enter_sleep()` does not return; CPU reboots on wake.
4. **Light sleep** — `enter_sleep()` returns with the wake cause; services
   are restarted, and `unlock()` or `on_measurement_timer()` is called
   depending on whether the user pressed a button or the timer expired.

### Duration

`compute_sleep_duration_ms()` returns the minimum of the measurement
interval and the display refresh interval (if display refresh is enabled).

## GPS Active Logic

`is_gps_active()` determines whether GPS data should be used:

| GpsMode | Result |
|---|---|
| `AlwaysOff` | `false` |
| `AlwaysOn` | `true` |
| `OnWhenTracking` | `_tracking_active` |

GPS hardware is always powered on and the GPS task always runs. This method
only controls whether `GpsFixUpdate` events update the cached GPS data.

## Iteration Count

`compute_iterations()` calculates the number of sensor averaging iterations:

```
iterations = measurement_interval_seconds * 1000 / ITERATION_INTERVAL_MS
```

Where `ITERATION_INTERVAL_MS` is 2000 ms (matching `SensorManager`).
Minimum of 1 iteration is enforced. The first measurement on boot always
uses 1 iteration for a fast initial reading.

## Session ID Generation

`generate_session_id()` maintains a persistent NVS counter that wraps
within the 5-digit range (10000–99999). Each call increments the counter,
saves it to NVS, and returns the new value. IDs are unique across power
cycles.

## Required Service Additions

This implementation required two additions to existing services:

### UIManager::apply_to_settings()

Added to `go_ui.h` / `go_ui.cpp`. Converts internal option indices back to
`GoSettings` field values — the reverse of `sync_settings()`. Covers:
units, PM display, display interval, PM interval, other sensor interval,
GPS mode, operating mode, and auto-lock timeout.

### StorageService::read_cache()

Added to `go_storage.h` / `go_storage.cpp`. Copies all cached `MeasuresAGo`
entries (oldest first) into a caller-provided buffer. Used by
`build_context()` to provide chart data to the UIManager.

### DisplayService TEST_HOST stub

Added to `go_display.h` in the `#else` branch of the `#ifndef TEST_HOST`
guard. Provides a no-op `DisplayService` class with matching method
signatures so the Orchestrator compiles without conditional compilation in
host test builds.

## Design Decisions

### Queue Timeout Polling

Timer deadlines are tracked with `RTOS::get_time_ms()` timestamps rather
than dedicated `esp_timer` or FreeRTOS software timers. This eliminates
platform dependencies, avoids callback-to-queue indirection, and centralizes
all timer logic for host testability.

### UI Actions via Direct Return

The primary path for UI actions (start tracking, change mode, etc.) is
through `UIManager::handle_input()` returning `UIActionResult`. The
`EventType` enum values for these actions are reserved for future
programmatic triggers (e.g., BLE commands).

### auto_lock_seconds vs inactivity_timeout_seconds

The inactivity timer uses `GoSettings::auto_lock_seconds` because this is
the field controlled by the UI "Auto Lock" setting and persisted correctly
through `save_go_settings()`. The `inactivity_timeout_seconds` field exists
in `GoSettings` but is not connected to any UI control.

### Invalid Sentinel Initialization

`_latest_measures` is initialized to invalid sentinel values (not zero)
using a `make_invalid_measures()` helper. This ensures the display shows
placeholder indicators rather than misleading zeros before the first
measurement completes.
