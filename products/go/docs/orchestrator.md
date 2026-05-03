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

The orchestrator is constructed by `GoApp` after all services are
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
    .ble_service     = ble_service,
};

Orchestrator orchestrator(event_queue, services, settings, config_store, serial);
orchestrator.init(cause);                         // fresh boot (default BootHandoff)
// or:
BootHandoff handoff{};
handoff.display_painted = true;
handoff.initial_lock_state = LockState::Unlocked;
orchestrator.init(WakeCause::Button, handoff);    // button-wake path
orchestrator.run();  // never returns
```

## `init()` — Boot Initialization

```cpp
void init(WakeCause cause, const BootHandoff &handoff = {});
```

`init()` is called once before `run()`. It restores persisted state, sets
timer baselines, requests the first measurement, and kicks the display.
The `BootHandoff` struct replaces the old `(bool already_painted,
const RtcDisplaySnapshot *snapshot)` parameters with explicit fields for
each dimension of boot state.

### RTC State Restoration

RTC state (`_behavior`, `_gps_enabled`, `_tracking_active`,
`_tracking_session_id`) is restored for all non-PowerOn wake causes:

```cpp
if (cause != WakeCause::PowerOn) {
    // Restore from RTC — both Timer and Button wakes have valid state
}
```

Previously, RTC state was only restored for `WakeCause::Button`. The
generalization is needed because fast-path promotion sends `WakeCause::Timer`
to the orchestrator, and the device was in a sleep cycle with persisted state.

### Lock State and Display

The orchestrator's initial lock state and display behavior are driven by
`BootHandoff` fields, not by wake-cause-specific branches:

**`initial_lock_state == Unlocked` + `display_painted == true`:**

The display already shows the correct unlocked UI. `init()` sets
`_lock_state = Unlocked` directly (bypasses `unlock()` to avoid a redundant
`update_display()`), arms the "Unlocked" snackbar timer, and sets
`_last_input_ms`.

**`initial_lock_state == Unlocked` + `display_painted == false`:**

The display hasn't been painted yet or shows stale content. `init()` calls
`unlock()` which triggers `update_display()` to paint the unlocked frame.

**`initial_lock_state == Locked` (default):**

No lock state change. Device stays locked.

### Cached Measures Seeding

`init()` seeds `_cached_measures` from the handoff in priority order:

1. `fast_path_measures` (fresh data from fast-path measurement) — highest priority
2. `display_snapshot` (stale RTC snapshot from last sleep) — fallback
3. Neither set — `_cached_measures` stays at invalid sentinels

### Measurement Completed

When `handoff.measurement_completed == true`, the orchestrator sets
`_first_measurement_done = true` and skips the initial measurement request.
This allows the sleep-too-short promotion case to immediately attempt sleep
on the next event loop iteration.

### Route Resumption

If `_tracking_active` is true after RTC state restoration, the orchestrator
calls `storage.start_route(_tracking_session_id)` to reopen the route file
in append mode.

## Application State

The orchestrator owns the authoritative application state:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `_mode` | `OperatingMode` | `Portable` | Portable / Stationary / Offline |
| `_behavior` | `Behavior` | `Idle` | Tracking / Idle / Shutdown |
| `_lock_state` | `LockState` | `Locked` | Locked / Unlocked |
| `_gps_enabled` | `bool` | `true` | Whether GPS data is used (derived from `GpsMode` setting) |
| `_tracking_active` | `bool` | `false` | True while a route is being logged |
| `_tracking_session_id` | `uint32_t` | `0` | 5-digit session ID; 0 = no active session |

On fresh boot (`PowerOn`), defaults are used. On wake from deep sleep (`Timer`
or `Button`), state is restored from RTC memory via
`PowerService::load_state()`.

## Event Loop

The `run()` method is an infinite loop using queue-timeout polling for timers:

1. **Sleep check** — when locked and the first measurement is done, attempt
   to enter deep sleep. Returns only when sleep conditions are not met (mode
   not Offline, or `sleep_ms < deep_sleep_threshold_ms`).
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
| PM pre-wake | `measure_interval - CONFIG_SENSOR_WARMUP_DURATION_MS` | Not Offline, interval ≥ `pm_sleep_threshold_ms`, prepare not yet sent |
| Sensor (all groups) | `measure_interval_seconds * 1000` | Always |
| BMS poll + watchdog | `BMS_POLL_INTERVAL_MS` (5000 ms) | Always |
| External watchdog | `EXT_WDT_INTERVAL_MS` (60000 ms) | Always |
| Inactivity | `auto_lock_seconds * 1000` | Unlocked and auto-lock > 0 |
| Snackbar refresh | `SNACKBAR_DURATION_MS + 200` (one-shot) | While snackbar is active |

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
| `MeasurementTimer` | `check_timers()` (legacy event, re-checks all timers) |
| `WakeFromSleep` | No-op (handled in `init()`) |
| `BleConnected` | Push current status/config, dismiss passkey overlay |
| `BleDisconnected` | Dismiss passkey overlay |
| `BleConfigWrite` | Decode config/command write and apply it |
| `BleHistoryWrite` | Decode history export request and delegate to BLE service |
| `BlePairingRequest` | Show passkey overlay |
| `BleAuthComplete` | Dismiss passkey overlay |
| `Co2CalibrationDone` | Show result snackbar, notify BLE command result, update display |

## Input Handling

`on_input()` processes input events with priority:

1. **Long press ButtonPower** — `shutdown()` (any lock state)
2. **Long press ButtonBoot** — `factory_reset()`, then reboot on success
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
session ID (random, range 10000–99999), opens/closes the route file, and
toggles `_behavior` between `Tracking` and `Idle`.

### change_mode()

Updates `_mode`, manages the BLE lifecycle for Portable mode, and shows a
snackbar. WiFi/HTTP server logic is still deferred.

### apply_settings_change()

Called when the UI signals a setting was changed. Calls
`UIManager::apply_to_settings()` to convert internal option indices back to
`GoSettings` fields, persists to NVS via `save_go_settings()`, and
propagates runtime changes (GPS posting interval, GPS enabled flag, sensor
timer rescheduling).

### clear_data()

Stops tracking if active, clears the temporary RTC-backed chart cache,
deletes all persisted route files from NAND, refreshes BLE status when a
client is connected, shows a snackbar, and returns success/failure.

### factory_reset()

Calls `clear_data()`, restores default `GoSettings`, deletes all stored BLE
bonds, resets runtime state back to Portable + Idle + Locked, updates the
display, and returns success/failure. The caller reboots the ESP on success.

### shutdown()

Stops tracking if active, backs up the cache, shows the shutdown screen,
waits for the display refresh (500 ms), then calls
`PowerService::shutdown()` (BMS ship mode — does not return).

## Display Update

### `update_display()`

Builds a `BuildContext` from cached state and asks the UIManager to produce
a `DisplayValues` snapshot:

1. Clear expired snackbar
2. `build_context()` — convert cached `MeasuresAGo` to `Measures`, read
   chart cache, extract battery info, and status flags
3. `UIManager::build_values(ctx)` — produce `DisplayValues`
4. `DisplayService::update(values)` — non-blocking render submission
5. If a snackbar is active and no refresh timer is pending, schedule a
   one-shot `_snackbar_refresh_deadline_ms` to guarantee the snackbar is
   visually cleared even if no other events trigger `update_display()`

The `BuildContext` requires a `const Measures &` reference. The orchestrator
maintains a `mutable Measures _display_measures` member that is populated
from the cached `MeasuresAGo` each time `build_context()` is called.

### Background Display Suppression

Display-update call sites are split into two categories:

**User-initiated** — call `update_display()` directly (always repaint):
`on_input()`, `lock()`, `unlock()`, `start_tracking()`, `stop_tracking()`,
`change_mode()`, `clear_data()`, `factory_reset()`, `save_tag()`,
`shutdown()`, `on_co2_calibration_done()`, `on_ble_pairing_request()`.

**Background** — call `request_background_display_update()`:
`on_sensor_data()`, `on_ble_connected()`, `on_ble_disconnected()`,
`on_ble_auth_complete()`, `on_ble_config_write()` (Set branch),
`on_bms_status_timer()`, snackbar refresh timer in `check_timers()`.

`request_background_display_update()` delegates to
`UIManager::is_on_menu_screen()` to decide whether to suppress:

```cpp
void Orchestrator::request_background_display_update() {
  if (!_svc.ui_manager.is_on_menu_screen()) {
    update_display();
  }
}
```

When the user is on any menu-navigation screen (MainMenu, Settings,
SettingsChoice, TagList, Confirm, About), background events still update
data caches, send BLE notifications, etc. — only the e-paper refresh is
skipped. The display catches up on the next user-initiated repaint (input,
lock/unlock, or returning to Home).

The orchestrator does not choose refresh tiers (Full/Fast/Partial). That
decision belongs entirely to `DisplayService::update()`.

## Sleep Cycle

### Entry

`try_enter_sleep()` is called at the top of each loop iteration when the
device is locked and the first measurement is complete:

1. `PowerService::decide_sleep()` determines the sleep type (`None` or `Deep`)
   and the adjusted sleep duration in one call. It computes
   `min(enabled intervals) - awake_ms`. Non-Offline modes and short intervals
   (< `deep_sleep_threshold_ms`) return `{None, 0}`.
2. If `None`: return immediately — the main loop continues normally.
3. If `Deep`: call `prepare_for_sleep()`, then `enter_sleep()`.
   `enter_sleep()` does not return; CPU reboots on wake.

### `prepare_for_sleep()`

```
1. Final display update with wait=true (blocks until e-paper refresh done)
2. save_rtc_display_snapshot(values) — persist sensor values, battery,
   status flags, rendering settings to RTC memory for next button wake
3. Stop services: BLE, sensor producer, GPS, input, display worker
4. display_service.deep_sleep() — put SSD1680 into sleep mode 1 (<1 µA)
5. storage.backup_cache() — persist chart data to RTC memory
6. power_service.save_state(snapshot_state()) — persist app state
7. power_service.reset_ext_watchdog() — maximize timeout window during sleep
```

`save_rtc_display_snapshot()` is called after `update(values, true)` so the
snapshot reflects exactly what was last rendered. It is intentionally before
`stop()` — the values are still valid at that point. `deep_sleep()` is called
after `stop()` to ensure the worker task is no longer using the SPI bus.

## GPS Active Logic

`is_gps_active()` determines whether GPS data should be used:

| GpsMode | Result |
|---|---|
| `AlwaysOff` | `false` |
| `AlwaysOn` | `true` |
| `OnWhenTracking` | `_tracking_active` |

GPS hardware is always powered on and the GPS task always runs. This method
only controls whether `GpsFixUpdate` events update the cached GPS data.

## Sensor Scheduling

The orchestrator maintains a single timer (`_last_measurement_ms`).
`check_timers()` fires when `measure_interval_seconds` elapses, always
requesting `SensorGroup::All` with a single
`request_measurement(1, SensorGroup::All)` call.

When the interval setting changes, `reschedule_sensor_timer()` resets the
baseline to `now` and reconciles PM sensor power with the new interval:
powers off if the new interval crosses above `pm_sleep_threshold_ms`,
powers on if it crosses below.  If the interval is unchanged, the
baseline and PM power are not touched.

Iterations are always 1 — AGo sensors perform internal averaging, and the
per-iteration 2 s delay is skipped for single iterations.

`on_sensor_data()` always overwrites all fields in `_cached_measures`
and, in non-Offline modes with a long enough interval, powers off the
PM sensor via `set_pm_power(false)` to save fan current until the next
pre-wake timer fires.  Sensor failures are immediately visible (display
shows dashes) rather than masked by stale cached data.

### PM Sensor Power-Cycling

When `mode != Offline` and `measure_interval_seconds * 1000 >=
pm_sleep_threshold_ms`, the orchestrator power-cycles the SPS30 between
measurements.  A `_pm_prepare_sent` flag prevents duplicate pre-wake
signals within the same measurement cycle; it is reset when the
measurement timer fires.

See [Power Management — PM Sensor Sleep](power_management.md#pm-sensor-sleep-active-mode-power-cycling)
for the full cycle, edge cases, and method documentation.

## Session ID Generation

`generate_session_id()` uses the shared `generate_random_number(5)` helper
from `airgradient-common` and returns a 5-digit ID in the range 10000–99999.
`0` remains reserved as the "no active session" sentinel.

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
`EventType` enum values for these actions also serve as programmatic
triggers — for example, BLE `start_tracking` / `stop_tracking` commands
dispatch through the same `start_tracking()` / `stop_tracking()` methods.

### auto_lock_seconds vs inactivity_timeout_seconds

The inactivity timer uses `GoSettings::auto_lock_seconds` because this is
the field controlled by the UI "Auto Lock" setting and persisted correctly
through `save_go_settings()`. The `inactivity_timeout_seconds` field exists
in `GoSettings` but is not connected to any UI control.

### Invalid Sentinel Initialization

`_cached_measures` is initialized to invalid sentinel values (not zero)
using a `make_invalid_measures()` helper. This ensures the display shows
placeholder indicators rather than misleading zeros before the first
measurement completes.
