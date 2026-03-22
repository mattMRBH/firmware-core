# Power Management — Implementation Spec

Product-specific power management for AirGradient Go. Handles BMS status
polling, battery monitoring, sleep cycle management, RTC state persistence, and
shutdown. Called synchronously by the orchestrator — no independent task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_power.h` | `PowerService` class declaration |
| `products/go/main/go_power.cpp` | BMS polling, sleep entry/exit, RTC state, fast-path boot |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `BQ25XX` | `airgradient-bms` (driver) | BMS I2C reads, charging status, QoN shutdown |
| `gpio::Hal` | `airgradient-gpio` | Configure GPIO wake sources for deep sleep |
| `go_types.h` | product | `RtcAppState`, `WakeCause`, `LockState`, `Behavior` |
| `go_settings.h` | product | `GoSettings` for interval-based sleep decisions |
| ESP-IDF sleep API | ESP-IDF | `esp_sleep_*` functions (deep/light sleep) |
| RTOS | `airgradient-common` | `RTOS::get_time_ms()` |

## BMS Integration

The `BQ25XX` driver implements the `BmsDevice` HAL interface from
`airgradient-bms`. The power service uses the concrete `BQ25XX` class
directly for:

- `read_telemetry(BmsTelemetry&)` — battery + charging voltage
- `get_battery_percentage(float*)` — estimated SOC
- `get_charging_status()` — charging state enum (`BmsChargingState`)
- `update_watchdog()` — periodic WD reset
- `enter_ship_mode()` — QoN shutdown (HAL method exists but driver returns
  `false` until the register sequence is implemented)
- `feature_ship_available()` — reports whether the driver supports ship mode
  (currently returns `false`)

### PowerSnapshot Struct

Product-specific struct that aggregates BMS data for the orchestrator and
display:

```cpp
struct PowerSnapshot {
    float battery_voltage    = BmsInvalid::VOLT;
    float charging_voltage   = BmsInvalid::VOLT;
    float battery_percentage = -1.0f;
    BmsChargingState charging_status = BmsChargingState::Unknown;
    bool critical            = false;  // below critical threshold
};
```

### Critical Battery Threshold

When battery percentage drops below a configurable threshold (e.g. 5%), the
`critical` flag is set. The orchestrator can use this to:
- Show a low battery warning on display
- Auto-shutdown to protect the battery

The critical threshold is a named constant, not a user setting:

```cpp
static constexpr float BATTERY_CRITICAL_PERCENT = 5.0f;
```

## Class Design

```cpp
#pragma once

#include "drivers/bq25xx/bq25xx.h"
#include "types/bms_types.h"
#include "airgradient_gpio.h"
#include "go_settings.h"
#include "go_types.h"

#include <cstdint>

struct PowerSnapshot {
    float battery_voltage    = BmsInvalid::VOLT;
    float charging_voltage   = BmsInvalid::VOLT;
    float battery_percentage = -1.0f;
    BmsChargingState charging_status = BmsChargingState::Unknown;
    bool critical            = false;
};

class PowerService {
  public:
    struct Config {
        int pin_wake_button_power;     // GPIO for deep sleep wake (Button 1)
        int pin_wake_button_boot;      // GPIO for deep sleep wake (Button 2)
        int deep_sleep_threshold_ms = 5000;  // min interval for deep sleep
    };

    PowerService(BQ25XX &bms, const gpio::Hal &gpio, const Config &config);

    // --- BMS operations (called by orchestrator on timer) ---

    /// Poll BMS for current status. Fast I2C read, non-blocking.
    PowerSnapshot poll_bms();

    /// Reset BMS watchdog. Must be called periodically (< 10s interval).
    bool reset_watchdog();

    /// Trigger BMS QoN (ship mode). Device powers off. Does not return.
    void shutdown();

    // --- RTC state persistence ---

    /// Save application state to RTC memory before sleep.
    void save_state(const RtcAppState &state);

    /// Load application state from RTC memory after wake.
    RtcAppState load_state() const;

    // --- Sleep cycle ---

    /// Determine the appropriate sleep type based on the next wake interval.
    enum class SleepType { None, Light, Deep };
    SleepType evaluate_sleep(const GoSettings &settings,
                             LockState lock_state) const;

    /// Enter sleep. Configures wake sources and enters deep or light sleep.
    /// For deep sleep: does not return (CPU reboots on wake).
    /// For light sleep: returns after wake, with the wake cause.
    WakeCause enter_sleep(SleepType type, uint32_t sleep_duration_ms);

    // --- Boot path ---

    /// Determine wake cause on boot. Call early in app_main.
    static WakeCause get_wake_cause();

    /// Returns true if this boot is a fast-path timer wake
    /// (locked + timer wake, eligible for abbreviated boot).
    static bool is_fast_path_wake(WakeCause cause, const RtcAppState &state);

  private:
    BQ25XX &_bms;
    const gpio::Hal &_gpio;
    Config _config;

    void configure_wake_sources(uint32_t timer_ms);
};
```

## RTC State Persistence

`RtcAppState` is stored in RTC memory using the `RTC_DATA_ATTR` attribute.
The PowerService provides save/load methods, but the actual storage is a
file-scope static variable:

```cpp
// In go_power.cpp
RTC_DATA_ATTR static RtcAppState s_rtc_state;
RTC_DATA_ATTR static bool s_rtc_state_valid = false;  // set true on first save
```

Under `TEST_HOST`, `RTC_DATA_ATTR` is defined away (same pattern as
`RtcPayloadCacheStorage`).

## Sleep Cycle Logic

### evaluate_sleep()

```
evaluate_sleep(settings, lock_state):
    if lock_state == Unlocked:
        return SleepType::None      // never sleep while unlocked

    // Calculate minimum interval until next needed action
    next_wake_ms = settings.measurement_interval_seconds * 1000

    if settings.display_refresh_interval_seconds > 0:
        display_ms = settings.display_refresh_interval_seconds * 1000
        next_wake_ms = min(next_wake_ms, display_ms)

    if next_wake_ms >= deep_sleep_threshold_ms:
        return SleepType::Deep
    else:
        return SleepType::Light
```

### enter_sleep()

```
enter_sleep(type, sleep_duration_ms):
    configure_wake_sources(sleep_duration_ms)

    if type == Deep:
        esp_deep_sleep_start()
        // does not return

    if type == Light:
        esp_light_sleep_start()
        cause = determine wake cause from esp_sleep_get_wakeup_cause()
        return cause
```

### configure_wake_sources()

```
configure_wake_sources(timer_ms):
    // Timer wake
    esp_sleep_enable_timer_wakeup(timer_ms * 1000)  // microseconds

    // GPIO wake: Button Power and Button Boot
    // Configure as EXT0 or EXT1 wake source depending on available pins
    esp_sleep_enable_ext0_wakeup(pin_wake_button_power, LOW)
    // Or use ext1 for multiple GPIO wake sources
```

GPIO wake source configuration is platform-specific (ESP32 variant dependent).
The exact API (`ext0`, `ext1`, or `gpio_wakeup`) depends on the target chip.

## Fast-Path Boot

The fast-path avoids starting the full event loop on timer wakes while locked.
It is implemented in `app_main` using PowerService static methods:

```
app_main():
    WakeCause cause = PowerService::get_wake_cause()
    RtcAppState state = power_service.load_state()

    if PowerService::is_fast_path_wake(cause, state):
        // Minimal initialization
        init I2C bus
        init sensor drivers
        init SPI + NAND mount

        // One-shot measurement
        SensorManager sm(sensors)
        Measures m = sm.start_measures(1)

        // One-shot GPS (if tracking)
        GpsData gps;
        if state.tracking_active && state.gps_enabled:
            gps = gps_read_once(serial, baud, timeout)

        // Store
        storage.cache_measurement(m)
        if state.tracking_active:
            storage.append_route_point({time(nullptr), gps, m})

        // Update display
        display.update_dashboard(m, gps, bms_status)

        // Back to sleep
        power_service.save_state(state)
        power_service.enter_sleep(SleepType::Deep, next_interval_ms)
        // does not return

    // Full boot path (fresh power-on or button wake)
    full_initialization()
    enter_event_loop()
```

### is_fast_path_wake()

```cpp
bool PowerService::is_fast_path_wake(WakeCause cause,
                                     const RtcAppState &state) {
    return cause == WakeCause::Timer && state.lock_state == LockState::Locked;
}
```

Only timer wakes while locked qualify. Button wakes always go through the full
boot path (user wants to interact).

## Watchdog Management

The BQ25XX has a hardware watchdog that must be reset periodically (every 10
seconds per datasheet). The orchestrator calls `reset_watchdog()` as part of
its measurement timer cycle. If the measurement interval is longer than 10
seconds, the orchestrator must set up a separate periodic call.

During deep sleep, the watchdog is not reset. The BQ25XX behavior on watchdog
expiry during sleep depends on chip configuration (typically resets charge
parameters to defaults). This may need investigation during hardware bring-up.

## Shutdown Flow

When the orchestrator receives a long-press on Button Power (shutdown request):

```
1. Orchestrator calls storage.end_route()       // close any open route
2. Orchestrator calls storage.backup_cache()     // save chart data to RTC
   (note: RTC data won't survive QoN power-off, but good practice)
3. Orchestrator calls display.show_shutdown()     // show shutdown indicator
4. Orchestrator calls power_service.shutdown()    // BMS QoN
   // Device powers off. GPS module powers off. Everything off.
```

On next power-on (button press on BMS), the device does a fresh boot.
`s_rtc_state_valid` will be false (RTC memory lost), so all state initializes
to defaults.

## Platform Abstraction

The sleep APIs (`esp_sleep_*`) are ESP-IDF specific. For host testing:

- `evaluate_sleep()` and `is_fast_path_wake()` are pure logic — testable
  without any platform dependency
- `enter_sleep()`, `configure_wake_sources()`, `get_wake_cause()`, and
  `shutdown()` are wrapped in `#ifndef TEST_HOST` guards
- `save_state()` and `load_state()` work under `TEST_HOST` (RTC_DATA_ATTR
  defined away, becomes regular static variable)
- `poll_bms()` calls the BQ25XX driver which requires I2C — mock in tests

## Testability

| Function | Testable on Host? | Notes |
|---|---|---|
| `evaluate_sleep()` | Yes | Pure logic, no platform calls |
| `is_fast_path_wake()` | Yes | Pure logic |
| `poll_bms()` | Yes (with mock) | Mock BQ25XX for I2C |
| `save_state()` / `load_state()` | Yes | RTC_DATA_ATTR defined away |
| `enter_sleep()` | No | ESP-IDF sleep APIs |
| `shutdown()` | No | BMS hardware command |
| `get_wake_cause()` | No | ESP-IDF wake cause query |
| `reset_watchdog()` | Yes (with mock) | Mock BQ25XX |
