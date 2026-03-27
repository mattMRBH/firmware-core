# Power Management Service

Product-specific power management for AirGradient Go. Handles BMS status
polling, battery monitoring, sleep cycle management, RTC state persistence,
and shutdown. Called synchronously by the orchestrator — no independent task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_power.h` | `PowerSnapshot` struct, `PowerService` class declaration |
| `products/go/main/go_power.cpp` | BMS polling, sleep entry/exit, RTC state, fast-path boot logic |

## Dependencies

| Dependency | Component | Usage |
|---|---|---|
| `BmsDevice` | `airgradient-bms` (HAL) | BMS telemetry, status, battery %, watchdog reset |
| `gpio::Hal` | `airgradient-gpio` | Configure GPIO wake sources for deep sleep |
| `go_types.h` | product | `RtcAppState`, `WakeCause`, `LockState` |
| `go_settings.h` | product | `GoSettings` for interval-based sleep decisions |
| `esp_sleep.h` | ESP-IDF | `esp_sleep_*` functions (deep/light sleep) |

## PowerSnapshot Fields

`PowerSnapshot` aggregates BMS data for the orchestrator and display.  All
fields default to invalid sentinels (`BmsInvalid::VOLT` / `-1.0f` / `false`).

| Field | Type | Sentinel | Description |
|---|---|---|---|
| `battery_voltage` | `float` | `-1.0f` | Battery voltage (V) from `BmsTelemetry::battery_voltage` |
| `charging_voltage` | `float` | `-1.0f` | Charging/bus voltage (V) from `BmsTelemetry::charging_voltage` |
| `battery_percentage` | `float` | `-1.0f` | Estimated SOC (0--100%) from `get_battery_percentage()` |
| `charging_status` | `BmsChargingState` | `Unknown` | Enumerated charging state |
| `critical` | `bool` | `false` | Set when `battery_percentage` is valid and below `BATTERY_CRITICAL_PERCENT` (5 %) |

## Critical Battery Threshold

```cpp
static constexpr float PowerService::BATTERY_CRITICAL_PERCENT = 5.0f;
```

This is a **fixed constant**, not a user-configurable setting.  When the
orchestrator receives a `PowerSnapshot` with `critical == true` it should:

1. Show a low-battery warning on the display.
2. Initiate an automatic shutdown to protect the battery cell.

## PowerService Config

Passed to the constructor; provides pin assignments and the deep-sleep
threshold:

| Field | Type | Default | Description |
|---|---|---|---|
| `pin_wake_button_power` | `int` | — | GPIO number for Button Power deep-sleep wake |
| `pin_wake_button_boot` | `int` | — | GPIO number for Button Boot deep-sleep wake (`-1` on ESP32-C5 — GPIO28 is not RTC-capable) |
| `pin_ext_wdt` | `int` | `-1` | External watchdog GPIO (`-1` = disabled); pulsed HIGH 20 ms on reset |
| `deep_sleep_threshold_ms` | `int` | `5000` | Minimum next-wake interval (ms) to prefer deep sleep over light sleep |

## Sleep Type Selection

`decide_sleep(settings, lock_state, mode, awake_ms)` is pure logic (no
platform calls, testable on host). Returns `SleepDecision {type, duration_ms}`:

```
Not Offline mode → {None, 0}   (only Offline mode sleeps)
Unlocked         → {None, 0}   (never sleep while user is active)

sleep_ms = min(enabled intervals) - awake_ms   (clamped to 0)

sleep_ms >= deep_sleep_threshold_ms → {Deep, sleep_ms}
sleep_ms <  deep_sleep_threshold_ms → {Light, sleep_ms}
```

`min(enabled intervals)` is the minimum of `pm_interval_seconds`,
`other_sensor_interval_seconds`, and `display_refresh_interval_seconds`
(excluding disabled intervals where value is 0). Falls back to 60 s if all
are disabled. `awake_ms` is subtracted so the total cycle (awake + sleep)
matches the configured interval.

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

The BMS device has a hardware watchdog that must be reset at least every **10 seconds**.
The orchestrator calls `reset_watchdog()` on each measurement timer tick.

If the minimum sensor interval exceeds 10 s, the orchestrator must schedule a
separate periodic call to `reset_watchdog()` to avoid watchdog expiry.

During deep sleep the watchdog is **not** reset.  On expiry the BMS typically
resets charge parameters to defaults; actual behavior should be verified during
hardware bring-up.

## Shutdown (QoN / Ship Mode)

`shutdown()` triggers BMS QoN (ship mode) via `_bms.enter_ship_mode()`,
which writes the BQ25629 registers to cut power to the entire system.
The call should not return since the system loses power. If it does
(error or unsupported hardware), the method spins with `vTaskDelay` to
preserve the "does not return" contract.

Shutdown sequence called by the orchestrator on a Button Power long-press:

1. `storage.end_route()` — close any open route file
2. `storage.backup_cache()` — save chart data to RTC memory
3. `display.show_shutdown()` — show shutdown indicator
4. `power_service.shutdown()` — BMS QoN (does not return)

## External Watchdog

An external hardware watchdog is connected to `PIN_EXT_WDT` (GPIO2). Two
methods on `PowerService` wrap the free functions from
`components/airgradient-common/common.h`:

| Method | Calls | Effect |
|---|---|---|
| `init_ext_watchdog()` | `ext_watchdog_init(gpio, pin)` | Configures pin as output, drives LOW |
| `reset_ext_watchdog()` | `ext_watchdog_reset(gpio, pin)` | Pulses pin HIGH for 20 ms, then LOW |

Both are no-ops when `Config::pin_ext_wdt < 0`.

Pulse points:

| When | Where | Purpose |
|---|---|---|
| Boot (fast + full) | `main.cpp` after PowerService construction | First pulse after wake/power-on |
| Every 60 s | Orchestrator `check_timers()` | Periodic keep-alive |
| Before sleep | Orchestrator `prepare_for_sleep()` | Maximize timeout window during sleep |

## Platform Abstraction Summary

| Method | Testable on Host? | Notes |
|---|---|---|
| `decide_sleep()` | Yes | Pure logic |
| `is_fast_path_wake()` | Yes | Pure logic |
| `poll_bms()` | Yes (mock BmsDevice) | I2C reads via driver |
| `reset_watchdog()` | Yes (mock BmsDevice) | |
| `save_state()` / `load_state()` | Yes | `RTC_DATA_ATTR` defined away |
| `enter_sleep()` | No | Calls `esp_sleep_*` |
| `configure_wake_sources()` | No | Calls `esp_sleep_*` |
| `get_wake_cause()` | No | Calls `esp_sleep_get_wakeup_cause()` |
| `shutdown()` | No | BMS hardware command |
| `init_ext_watchdog()` | Yes (mock gpio::Hal) | GPIO config via HAL |
| `reset_ext_watchdog()` | Yes (mock gpio::Hal) | GPIO pulse via HAL |
