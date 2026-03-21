# Power Management Service

Product-specific power management for AirGradient Go. Handles BMS status
polling, battery monitoring, sleep cycle management, RTC state persistence,
and shutdown. Called synchronously by the orchestrator — no independent task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_power.h` | `BmsStatus` struct, `PowerService` class declaration |
| `products/go/main/go_power.cpp` | BMS polling, sleep entry/exit, RTC state, fast-path boot logic |

## Dependencies

| Dependency | Component | Usage |
|---|---|---|
| `BQ25XX` | `airgradient-sensors` | BMS I2C reads, charging status, watchdog reset |
| `gpio::Hal` | `airgradient-gpio` | Configure GPIO wake sources for deep sleep |
| `go_types.h` | product | `RtcAppState`, `WakeCause`, `LockState` |
| `go_settings.h` | product | `GoSettings` for interval-based sleep decisions |
| `esp_sleep.h` | ESP-IDF | `esp_sleep_*` functions (deep/light sleep) |

## BmsStatus Fields

`BmsStatus` aggregates BMS data for the orchestrator and display.  All fields
default to invalid sentinels (`MeasuresInvalid::VOLT` / `-1.0f` / `false`).

| Field | Type | Sentinel | Description |
|---|---|---|---|
| `battery_voltage` | `float` | `-1.0f` | Battery voltage (V) from `BatteryMgmtData::volt_battery` |
| `charging_voltage` | `float` | `-1.0f` | Charging/bus voltage (V) from `BatteryMgmtData::volt_charging` |
| `battery_percentage` | `float` | `-1.0f` | Estimated SOC (0–100%) from `getBatteryPercentage()` |
| `charging_status` | `BQ25XX::ChargingStatus` | `Unknown` | Enumerated charging state |
| `critical` | `bool` | `false` | Set when `battery_percentage` is valid and below `BATTERY_CRITICAL_PERCENT` (5 %) |

## Critical Battery Threshold

```cpp
static constexpr float PowerService::BATTERY_CRITICAL_PERCENT = 5.0f;
```

This is a **fixed constant**, not a user-configurable setting.  When the
orchestrator receives a `BmsStatus` with `critical == true` it should:

1. Show a low-battery warning on the display.
2. Initiate an automatic shutdown to protect the battery cell.

## PowerService Config

Passed to the constructor; provides pin assignments and the deep-sleep
threshold:

| Field | Type | Default | Description |
|---|---|---|---|
| `pin_wake_button_power` | `int` | — | GPIO number for Button Power deep-sleep wake |
| `pin_wake_button_boot` | `int` | — | GPIO number for Button Boot deep-sleep wake |
| `deep_sleep_threshold_ms` | `int` | `5000` | Minimum next-wake interval (ms) to prefer deep sleep over light sleep |

## Sleep Type Selection

`evaluate_sleep()` is pure logic (no platform calls, testable on host):

```
Unlocked  → SleepType::None   (never sleep while user is active)
Locked, next_wake_ms >= deep_sleep_threshold_ms → SleepType::Deep
Locked, next_wake_ms <  deep_sleep_threshold_ms → SleepType::Light
```

`next_wake_ms` is the minimum of `measurement_interval_seconds * 1000` and
`display_refresh_interval_seconds * 1000` (if display refresh is enabled).

## Sleep Entry

`enter_sleep(type, sleep_duration_ms)`:

1. Calls `configure_wake_sources(sleep_duration_ms)`:
   - Timer: `esp_sleep_enable_timer_wakeup()` (µs)
   - Buttons: `esp_sleep_enable_ext1_wakeup()` with a combined bitmask for
     both button pins (ESP32-C5 target uses EXT1; no EXT0 support on this chip)
2. `Deep`: calls `esp_deep_sleep_start()` — does **not** return.
3. `Light`: calls `esp_light_sleep_start()`, then returns `get_wake_cause()`.

## Wake Cause Mapping

`get_wake_cause()` maps `esp_sleep_get_wakeup_cause()` to `WakeCause`:

| ESP-IDF cause | `WakeCause` |
|---|---|
| `ESP_SLEEP_WAKEUP_TIMER` | `Timer` |
| `ESP_SLEEP_WAKEUP_EXT0/EXT1/GPIO` | `Button` |
| `ESP_SLEEP_WAKEUP_UNDEFINED` (first power-on) | `PowerOn` |

## Fast-Path Boot

`is_fast_path_wake(cause, state)` returns `true` when:

```cpp
cause == WakeCause::Timer && state.lock_state == LockState::Locked
```

When true, `app_main` should follow the abbreviated boot path: initialize
only the sensor bus and display, take one measurement, update the display, and
re-enter deep sleep — without starting the full event loop.

## RTC State Persistence

`RtcAppState` is stored in two RTC-memory variables in `go_power.cpp`:

```cpp
RTC_DATA_ATTR static RtcAppState s_rtc_state;
RTC_DATA_ATTR static bool s_rtc_state_valid = false;
```

- `save_state(state)` — copies `state` into `s_rtc_state` and sets the valid flag.
- `load_state()` — returns a copy of `s_rtc_state` if valid; otherwise returns
  a default-constructed `RtcAppState` (safe starting point for fresh power-on).

Under `TEST_HOST`, `RTC_DATA_ATTR` is defined away so the variables become
ordinary statics — `save_state()` / `load_state()` work identically.

## BMS Watchdog

The BQ25XX has a hardware watchdog that must be reset at least every **10 seconds**.
The orchestrator calls `reset_watchdog()` on each measurement timer tick.

If `measurement_interval_seconds > 10`, the orchestrator must schedule a
separate periodic call to `reset_watchdog()` to avoid watchdog expiry.

During deep sleep the watchdog is **not** reset.  On expiry the BMS typically
resets charge parameters to defaults; actual behavior should be verified during
hardware bring-up.

## Shutdown (QoN / Ship Mode)

`shutdown()` is intended to trigger BMS QoN (ship mode), which cuts power to
the entire system.

> **Note:** The current `BQ25XX` driver does not expose a QoN / ship-mode
> method.  `shutdown()` is a **stub** that logs the intent and spins until
> hardware support is added.  See the `TODO` comment in `go_power.cpp` and
> `components/airgradient-sensors/drivers/bq25xx/bq25xx.h` for the expected
> implementation path.

Shutdown sequence called by the orchestrator on a Button Power long-press:

1. `storage.end_route()` — close any open route file
2. `storage.backup_cache()` — save chart data to RTC memory
3. `display.show_shutdown()` — show shutdown indicator
4. `power_service.shutdown()` — BMS QoN (does not return)

## Platform Abstraction Summary

| Method | Testable on Host? | Notes |
|---|---|---|
| `evaluate_sleep()` | Yes | Pure logic |
| `is_fast_path_wake()` | Yes | Pure logic |
| `poll_bms()` | Yes (mock BQ25XX) | I2C reads via driver |
| `reset_watchdog()` | Yes (mock BQ25XX) | |
| `save_state()` / `load_state()` | Yes | `RTC_DATA_ATTR` defined away |
| `enter_sleep()` | No | Calls `esp_sleep_*` |
| `configure_wake_sources()` | No | Calls `esp_sleep_*` |
| `get_wake_cause()` | No | Calls `esp_sleep_get_wakeup_cause()` |
| `shutdown()` | No | BMS hardware command |
