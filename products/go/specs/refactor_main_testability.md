# Refactor main.cpp — GoBoard + GoApp Testability Spec

Extract application logic from `main.cpp` into a testable `GoApp` class,
backed by a `GoBoard` abstract factory / BSP interface for hardware object
creation.  `main.cpp` becomes a thin shell that constructs the real
board and runs the app.

## Problem

`main.cpp` (971 lines) mixes hardware object creation with application
logic.  The file contains three boot paths (fast path, button wake,
interactive), data transforms, handoff construction, and all `init_*`
helpers — none of which are host-testable.

The fast path alone is 194 lines of stateful, interruptible control flow
with multiple promotion decision points.  As the product grows, this file
will accumulate more logic without any test coverage.

The Orchestrator (event loop) is already well-tested (2900+ lines of
tests).  The gap is everything **before** the Orchestrator starts.

## Goals

- Make all boot-path decision logic host-testable
- Make the fast-path control flow (warmup, measure, GPS, sleep/promote)
  host-testable
- Make data transforms and handoff construction host-testable
- Reduce `main.cpp` to a thin shell with no logic
- Preserve exact runtime behavior — this is a refactor, not a rewrite
- Follow existing test patterns (link-time stubs, friend class access,
  Catch2 + Trompeloeil)

## Non-Goals

- Do not add abstract interfaces to product services (StorageService,
  PowerService, DisplayService, BleService).  They remain concrete classes
  with existing link-time stub and `#ifdef TEST_HOST` test patterns.
- Do not test hardware init wiring (`init_nvs`, `init_i2c_bus`,
  `init_gpio`, `init_spi_buses`, `init_bms`, `init_co2_sensor`).
  These are validated by hardware integration tests.
- Do not change the Orchestrator or any existing service.
- Do not change existing test files.

## Files

| File | Change |
|---|---|
| `products/go/main/go_board.h` | **New.** GoBoard abstract interface |
| `products/go/main/go_hardware_board.h` | **New.** GoHardwareBoard class declaration |
| `products/go/main/go_hardware_board.cpp` | **New.** GoHardwareBoard implementation (moved from main.cpp init helpers) |
| `products/go/main/go_app.h` | **New.** GoApp class + pure utility functions |
| `products/go/main/go_app.cpp` | **New.** GoApp implementation (moved from main.cpp boot paths) |
| `products/go/main/main.cpp` | **Rewritten.** Thin shell: construct board, construct app, run |
| `products/go/main/CMakeLists.txt` | Add `go_hardware_board.cpp`, `go_app.cpp` to `SRCS` |
| `products/go/tests/go_app.tests.cpp` | **New.** GoApp tests (fast path, boot selection, handoff, pure functions) |
| `products/go/tests/go_app_stubs.cpp` | **New.** Link-time stubs for GoApp test build |
| `products/go/tests/CMakeLists.txt` | Add `go_app_tests` target |

**Not touched:** `go_orchestrator.h/.cpp`, `go_power.h/.cpp`,
`go_display.h/.cpp`, `go_storage.h/.cpp`, `go_input.h/.cpp`,
`go_sensor_producer.h/.cpp`, `go_ble.h/.cpp`, `go_ui.h/.cpp`,
`go_settings.h/.cpp`, `go_ulp.h/.cpp`, `go_events.h`, `go_types.h`,
`board_config.h`, `gps/*`.  All existing test files are unchanged.

## Architecture

```text
main.cpp (thin shell — ~10 lines)
  └─ GoApp (all boot logic — host-testable)
       ├─ GoBoard (abstract factory — creates hardware objects)
       │    └─ GoHardwareBoard (real ESP-IDF implementation)
       └─ Orchestrator (already tested separately)
```

### Separation of concerns

| Layer | Responsibility | Testable on host? |
|---|---|---|
| **main.cpp** | Construct `GoHardwareBoard`, construct `GoApp`, call `run()` | No (hardware entry point) |
| **GoApp** | Boot path selection, fast-path logic, service construction, orchestrator launch, pure data transforms | **Yes** (via MockBoard + link-time stubs) |
| **GoBoard** | Abstract interface for hardware object creation and platform operations | N/A (interface) |
| **GoHardwareBoard** | All ESP-IDF init calls, driver creation, bus management, ISR setup | No (hardware-specific) |

## GoBoard Interface

`products/go/main/go_board.h`:

```cpp
#pragma once

#include "airgradient_gpio.h"
#include "go_types.h"

#include <cstdint>
#include <string>

// Forward declarations — avoid pulling full headers into the interface.
class BmsDevice;
class CapTouchSensor;
class ConfigStore;
class DisplayService;
class GpsDriver;
class PowerService;
class SensorManager;
class StorageService;
struct GoSettings;

/// Abstract factory / BSP interface for the AGo board.
///
/// Owns the lifecycle of every hardware-touching object.  GoApp calls
/// init methods in boot-path-specific order, then accesses services via
/// lazy accessors.
///
/// On target, GoHardwareBoard implements this with real ESP-IDF drivers.
/// In tests, MockBoard implements this with stubs and mocks.
struct GoBoard {
    virtual ~GoBoard() = default;

    // -----------------------------------------------------------------
    // Init methods (fine-grained, idempotent)
    //
    // Each initialises one subsystem.  Safe to call multiple times —
    // subsequent calls are no-ops.  Boot paths call these in the order
    // their hardware sequencing requires.
    // -----------------------------------------------------------------

    virtual void init_nvs() = 0;    ///< NVS flash
    virtual void init_buses() = 0;  ///< GPIO power enables + I2C bus + settling
    virtual void init_spi() = 0;    ///< SPI bus
    virtual void init_bms() = 0;    ///< BMS driver (requires buses)

    // -----------------------------------------------------------------
    // Convenience gate — calls all init methods above (skips what's done)
    // -----------------------------------------------------------------

    virtual void init_core() = 0;

    // -----------------------------------------------------------------
    // Lazy service accessors
    //
    // Create-on-first-call.  Objects are owned by the board and live for
    // the duration of the process (never freed — the app never returns).
    // Callers must ensure prerequisite init methods have been called.
    // -----------------------------------------------------------------

    virtual ConfigStore &config_store() = 0;
    virtual GoSettings load_settings() = 0;
    virtual BmsDevice &bms() = 0;
    virtual SensorManager &sensors(bool warm = false) = 0;
    virtual StorageService &storage() = 0;
    virtual DisplayService &display() = 0;
    virtual PowerService &power() = 0;

    // -----------------------------------------------------------------
    // Per-call factories (caller owns the returned object)
    // -----------------------------------------------------------------

    virtual GpsDriver *new_gps_driver() = 0;
    virtual CapTouchSensor *new_touch_sensor() = 0;

    // -----------------------------------------------------------------
    // Platform info
    // -----------------------------------------------------------------

    virtual std::string serial_number() = 0;
    virtual const char *firmware_version() = 0;
    virtual const gpio::Hal &gpio_hal() = 0;

    // -----------------------------------------------------------------
    // Hardware operations
    // -----------------------------------------------------------------

    virtual void release_gpio_holds() = 0;
    virtual void ulp_stop() = 0;
    virtual void ulp_start() = 0;

    // -----------------------------------------------------------------
    // Button ISR management
    //
    // The fast path uses a GPIO ISR to detect button presses during
    // blocking operations.  The board owns ISR setup/teardown;
    // GoApp reads the volatile flag.
    // -----------------------------------------------------------------

    /// Install a falling-edge ISR on the given pin.  When triggered,
    /// the ISR sets *flag to true.
    virtual void install_button_isr(int pin, volatile bool *flag) = 0;

    /// Remove the ISR and disable the interrupt on the given pin.
    virtual void remove_button_isr(int pin) = 0;
};
```

### Init method ordering by boot path

| Boot path | Init sequence |
|---|---|
| **Fast path** | `init_core()` → `sensors(warm)` → ... → `storage()` → `display()` → `power()` |
| **Button wake** | `init_spi()` → `display()` → early paint → `init_nvs()` + `init_buses()` + `init_bms()` → `sensors()` → ... |
| **Interactive** | `init_core()` → `sensors()` → `storage()` → `display()` → `power()` → ... |

`init_core()` is a convenience gate that calls `init_nvs()`, `init_buses()`,
`init_spi()`, and `init_bms()`, skipping any that have already been called.
The button-wake path calls `init_spi()` first (for early display paint),
then `init_nvs()`, `init_buses()`, `init_bms()` individually — `init_spi()`
is skipped internally because it was already done.

### Why these specific methods

All four init methods correspond to real hardware sequencing constraints:

- **NVS** must be ready before ConfigStore can read settings
- **Buses** (GPIO power enables + I2C) must be ready before I2C devices
  (BMS, sensors, touch) can be accessed.  The GPIO enable powers the
  sensor rail and the I2C settling delay is required for reliable
  communication.
- **SPI** must be ready before display and NAND flash
- **BMS** must be initialised before sensors because the BQ25629
  configures the PMID 5V rail that powers the SPS30 PM sensor

The service accessors (`sensors()`, `storage()`, `display()`, `power()`)
create their objects lazily.  GoHardwareBoard internally asserts that
prerequisite init methods have been called.

## GoHardwareBoard

`products/go/main/go_hardware_board.h`:

```cpp
#pragma once

#include "go_board.h"

#include <driver/i2c_master.h>

class BQ25629Bms;
class NvsConfigStore;

/// Real hardware implementation of GoBoard for the AGo board.
///
/// Wraps all ESP-IDF init calls, driver creation, and bus management.
/// Objects are heap-allocated and never freed (process lifetime).
class GoHardwareBoard : public GoBoard {
public:
    // --- Init methods ---
    void init_nvs() override;
    void init_buses() override;
    void init_spi() override;
    void init_bms() override;
    void init_core() override;

    // --- Lazy service accessors ---
    ConfigStore &config_store() override;
    GoSettings load_settings() override;
    BmsDevice &bms() override;
    SensorManager &sensors(bool warm) override;
    StorageService &storage() override;
    DisplayService &display() override;
    PowerService &power() override;

    // --- Per-call factories ---
    GpsDriver *new_gps_driver() override;
    CapTouchSensor *new_touch_sensor() override;

    // --- Platform ---
    std::string serial_number() override;
    const char *firmware_version() override;
    const gpio::Hal &gpio_hal() override;
    void release_gpio_holds() override;
    void ulp_stop() override;
    void ulp_start() override;
    void install_button_isr(int pin, volatile bool *flag) override;
    void remove_button_isr(int pin) override;

private:
    // Init tracking (idempotency)
    bool _nvs_ready = false;
    bool _buses_ready = false;
    bool _spi_ready = false;
    bool _bms_ready = false;

    // Bus handles
    i2c_master_bus_handle_t _i2c_bus = nullptr;

    // Owned objects (heap-allocated, never freed)
    NvsConfigStore *_config_store = nullptr;
    GoSettings _settings{};
    bool _settings_loaded = false;
    BQ25629Bms *_bms_driver = nullptr;
    SensorManager *_sensor_manager = nullptr;
    StorageService *_storage = nullptr;
    DisplayService *_display = nullptr;
    PowerService *_power = nullptr;
};
```

### Implementation notes

`go_hardware_board.cpp` contains the same logic as the current `main.cpp`
init helpers, reorganised into method bodies:

| Current function | Becomes |
|---|---|
| `init_nvs()` | `GoHardwareBoard::init_nvs()` |
| `init_gpio()` + `init_i2c_bus()` | `GoHardwareBoard::init_buses()` |
| `init_spi_buses()` | `GoHardwareBoard::init_spi()` |
| `init_bms(i2c_bus)` | `GoHardwareBoard::init_bms()` |
| `init_settings(ctx)` | Split: `init_nvs()` + `config_store()` + `load_settings()` |
| `init_sensors(ctx, warm)` | `GoHardwareBoard::sensors(warm)` |
| `init_storage(ctx)` | `GoHardwareBoard::storage()` |
| `init_display(ctx)` | `GoHardwareBoard::display()` |
| `init_power(ctx)` | `GoHardwareBoard::power()` |
| `init_core(ctx)` | `GoHardwareBoard::init_core()` |
| `init_co2_sensor(i2c_bus)` | Private helper called by `sensors()` |
| `BootContext` struct | Replaced by GoHardwareBoard member variables |

`init_core()` implementation:

```cpp
void GoHardwareBoard::init_core() {
    init_nvs();
    init_buses();
    init_spi();
    init_bms();
}
```

Each fine-grained method guards itself:

```cpp
void GoHardwareBoard::init_nvs() {
    if (_nvs_ready) return;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    _nvs_ready = true;
}
```

## GoApp

`products/go/main/go_app.h`:

```cpp
#pragma once

#include "go_board.h"
#include "go_display.h"
#include "go_types.h"
#include "measures_types.h"

class GoApp {
public:
    explicit GoApp(GoBoard &board);

    /// Main entry point.  Selects boot path and runs.
    /// Never returns on target.
    void run();

private:
    // --- Boot paths ---
    void run_fast_path(const RtcAppState &state);
    void run_button_wake_path(const RtcAppState &state);
    void run_interactive(WakeCause cause, BootHandoff handoff);

    // --- Testable fast-path core ---

    struct FastPathResult {
        enum class Outcome { Sleep, Promote };
        Outcome outcome;
        BootHandoff handoff;
        MeasuresAGo measures;
        bool has_measures;
        uint32_t sleep_duration_ms;   ///< Valid when outcome == Sleep
        bool sensors_warm;            ///< RTC persistence before sleep
    };

    FastPathResult execute_fast_path(const RtcAppState &state,
                                     const volatile bool &button_flag,
                                     const RtcDisplaySnapshot *snapshot,
                                     bool snapshot_valid);

    GoBoard &_board;

#ifdef TEST_HOST
    friend class GoAppTestAccess;
#endif
};

// ---------------------------------------------------------------------------
// Boot path selection
// ---------------------------------------------------------------------------

enum class BootPath { FastPath, ButtonWake, Interactive };

BootPath select_boot_path(WakeCause cause, const RtcAppState &state);

// ---------------------------------------------------------------------------
// Pure utility functions
// ---------------------------------------------------------------------------

/// Convert shared Measures to product-specific MeasuresAGo.
MeasuresAGo measures_to_ago(const Measures &m);

/// Build DisplayValues for the fast-path locked dashboard.
DisplayValues build_fast_path_display(const MeasuresAGo &measures,
                                      const GpsData &gps,
                                      const PowerSnapshot &bms,
                                      const GoSettings &settings,
                                      bool tracking_active);

/// Build DisplayValues for button-wake early paint (snapshot-based).
DisplayValues build_wake_values(const RtcDisplaySnapshot &snapshot,
                                bool snapshot_valid);

/// Determine whether GPS should be active based on settings and RTC state.
/// Used by all three boot paths — eliminates the duplicated inline check.
bool is_gps_active_at_boot(const GoSettings &settings,
                            const RtcAppState &state);
```

### GoApp::run()

`products/go/main/go_app.cpp`:

```cpp
void GoApp::run() {
    RTOS::delay_ms(100);
    WakeCause cause = PowerService::get_wake_cause();

    BootPath path = select_boot_path(cause, load_rtc_app_state());

    switch (path) {
    case BootPath::FastPath:
        run_fast_path(load_rtc_app_state());
        break; // never reached
    case BootPath::ButtonWake:
        run_button_wake_path(load_rtc_app_state());
        break; // never reached
    case BootPath::Interactive:
        AG_LOGI(TAG, "Serial number: %s", _board.serial_number().c_str());
        run_interactive(cause, {});
        break; // never reached
    }
}
```

Note: `PowerService::get_wake_cause()` is a static method that reads
ESP-IDF wake cause registers.  It has no GoBoard dependency and is
already stubbed under `TEST_HOST` via link-time replacement.
`load_rtc_app_state()` is a free function in `go_power.cpp`, also
stubbed at link time for tests.

### GoApp::run_fast_path()

Thin wrapper that handles ISR setup/teardown and sleep entry, delegating
the core logic to `execute_fast_path()`:

```cpp
void GoApp::run_fast_path(const RtcAppState &state) {
    AG_LOGI(TAG, "run_fast_path: entering fast-path boot (sensors_warm=%d)",
            state.sensors_warm);

    // ISR for button detection during blocking operations.
    volatile bool button_pressed = false;
    _board.install_button_isr(PIN_BUTTON_POWER, &button_pressed);

    _board.ulp_stop();

    // Load snapshot here so it stays on this (non-returning) stack.
    // execute_fast_path may reference it via pointer in the handoff.
    RtcDisplaySnapshot snapshot{};
    bool snapshot_valid = load_rtc_display_snapshot(&snapshot);

    auto result = execute_fast_path(state, button_pressed,
                                    &snapshot, snapshot_valid);

    _board.remove_button_isr(PIN_BUTTON_POWER);

    if (result.outcome == FastPathResult::Outcome::Sleep) {
        RtcAppState save = state;
        save.sensors_warm = result.sensors_warm;
        _board.power().save_state(save);

        _board.display().stop();
        _board.display().deep_sleep();
        _board.ulp_start();
        _board.power().enter_sleep(result.sleep_duration_ms);
        // Never returns — CPU reboots on wake.
    }

    // Promotion to interactive — wire fast_path_measures pointer into
    // result struct (lives on this non-returning stack).
    if (result.has_measures) {
        result.handoff.fast_path_measures = &result.measures;
    }

    AG_LOGI(TAG, "fast-path promoting to interactive (button=%d)",
            static_cast<int>(button_pressed));
    run_interactive(WakeCause::Timer, result.handoff);
}
```

### GoApp::execute_fast_path()

Contains the core fast-path logic.  Returns a `FastPathResult` instead of
calling `enter_sleep()` directly — this is the key testability seam.

The logic is identical to the current `run_fast_path()` in `main.cpp`,
with hardware access routed through `_board`:

```cpp
GoApp::FastPathResult GoApp::execute_fast_path(
    const RtcAppState &state,
    const volatile bool &button_pressed,
    const RtcDisplaySnapshot *snapshot,
    bool snapshot_valid) {

    const uint32_t boot_time_ms = static_cast<uint32_t>(RTOS::get_time_ms());
    GoSettings settings = _board.load_settings();

    // --- Core init ---
    _board.init_core();
    _board.release_gpio_holds();

    SensorManager &sm = _board.sensors(state.sensors_warm);

    bool promote = false;

    // --- Warmup (interruptible) ---
    if (state.sensors_warm) {
        RTOS::delay_ms(200);
    } else {
        const int warmup_iters =
            CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS;
        for (int i = 0; i < warmup_iters && !promote; i++) {
            uint64_t start = RTOS::get_time_ms();
            sm.warmup_step();

            if (button_pressed) { promote = true; break; }

            uint64_t elapsed = RTOS::get_time_ms() - start;
            if (elapsed < CONFIG_SENSOR_WARMUP_INTERVAL_MS) {
                RTOS::delay_ms(static_cast<uint32_t>(
                    CONFIG_SENSOR_WARMUP_INTERVAL_MS - elapsed));
            }
            if (button_pressed) { promote = true; break; }
        }
    }

    // --- One-shot measurement ---
    MeasuresAGo ago{};
    bool has_measures = false;
    if (!promote) {
        Measures measures = sm.start_measures(1, SensorGroup::All);
        measures.tvoc_nox.tvoc_index = measures.tvoc_nox.tvoc_raw;
        measures.tvoc_nox.nox_index = measures.tvoc_nox.nox_raw;
        ago = measures_to_ago(measures);
        has_measures = true;
        if (button_pressed) { promote = true; }
    }

    // --- One-shot GPS ---
    GpsData gps{};
    const bool gps_active = is_gps_active_at_boot(settings, state);
    if (!promote && state.tracking_active && gps_active) {
        auto *gps_driver = _board.new_gps_driver();
        gps = gps_read_once(*gps_driver, GPS_BAUD, 2000, button_pressed);
        if (button_pressed) { promote = true; }
    }

    // --- Storage + cache ---
    if (!promote) {
        StorageService &storage = _board.storage();
        storage.cache_measurement(ago);

        if (state.tracking_active) {
            float battery_pct = -1.0f;
            _board.bms().get_battery_percentage(&battery_pct);
            storage.start_route(state.tracking_session_id);
            RoutePoint point{};
            point.timestamp = time(nullptr);
            point.gps = gps;
            point.sensors = ago;
            point.battery_percentage = battery_pct;
            storage.append_route_point(point);
            storage.end_route();
        }
        _board.storage().backup_cache();
    }

    // --- Display + sleep decision ---
    if (!promote) {
        PowerService &power = _board.power();
        PowerSnapshot bms_snap = power.poll_bms();

        DisplayService &disp = _board.display();
        DisplayValues values = build_fast_path_display(
            ago, gps, bms_snap, settings, state.tracking_active);
        disp.init(values);

        uint32_t awake_ms =
            static_cast<uint32_t>(RTOS::get_time_ms()) - boot_time_ms;
        auto decision = power.decide_sleep(
            settings, LockState::Locked, OperatingMode::Offline, awake_ms);

        if (decision.type == PowerService::SleepType::Deep) {
            return {
                .outcome = FastPathResult::Outcome::Sleep,
                .handoff = {},
                .measures = ago,
                .has_measures = has_measures,
                .sleep_duration_ms = decision.duration_ms,
                .sensors_warm = power.should_hold_pm_sensor(decision.duration_ms),
            };
        }
        // Sleep too short — fall through to promotion.
        promote = true;
    }

    // --- Build promotion handoff ---
    //
    // NOTE: handoff.fast_path_measures is NOT set here.  The MeasuresAGo
    // value is returned in FastPathResult::measures.  The caller
    // (run_fast_path) sets fast_path_measures to point into the result
    // struct, which lives on the caller's non-returning stack.
    const bool button_caused = button_pressed;

    BootHandoff handoff{};
    handoff.measurement_completed = has_measures;
    // fast_path_measures left null — caller wires the pointer (see note above)

    if (button_caused) {
        handoff.initial_lock_state = LockState::Unlocked;
        handoff.suppress_wake_press = true;
        handoff.display_snapshot = snapshot_valid ? snapshot : nullptr;
        handoff.display_painted = false;
    } else {
        // Sleep too short — stay locked.  display.init() was called in
        // the sleep phase, so the display shows a correct locked frame.
        handoff.initial_lock_state = LockState::Locked;
        handoff.display_painted = true;
    }

    return {
        .outcome = FastPathResult::Outcome::Promote,
        .handoff = handoff,
        .measures = ago,
        .has_measures = has_measures,
        .sleep_duration_ms = 0,
        .sensors_warm = false,
    };
}
```

### BootHandoff pointer lifetime

`BootHandoff` contains two pointer members: `display_snapshot` and
`fast_path_measures`.  In the current code, these point to stack-local
variables in `run_fast_path()`.  After the refactor, both pointers are
wired by the non-returning caller rather than by `execute_fast_path()`:

- **`fast_path_measures`:** `execute_fast_path()` returns `MeasuresAGo`
  as a value in `FastPathResult::measures`.  It does NOT set
  `handoff.fast_path_measures` (it would dangle after return).  Instead,
  `run_fast_path()` sets `result.handoff.fast_path_measures =
  &result.measures` — the result struct lives on the non-returning
  stack, so the pointer is valid for the process lifetime.

- **`display_snapshot`:** `run_fast_path()` loads the `RtcDisplaySnapshot`
  on its own stack _before_ calling `execute_fast_path()`, and passes it
  as a parameter.  `execute_fast_path()` stores the pointer in the
  handoff for the button-promotion case.  Since the snapshot lives on
  the non-returning caller's stack, the pointer remains valid.

This two-level pattern keeps `execute_fast_path()` free of lifetime
hazards while preserving the same pointer semantics the Orchestrator
expects.

### GoApp::run_button_wake_path()

Same logic as current `run_button_wake_path()` in `main.cpp`, with
hardware access routed through `_board`:

```text
Phase 1:  _board.init_spi() → _board.display() → early paint → _board.ulp_stop()
Phase 2:  _board.init_nvs() → _board.init_buses() → _board.init_bms()
          → _board.sensors() → _board.new_touch_sensor() → _board.new_gps_driver()
          → _board.power() → start producer tasks
Phase 3:  _board.storage() → BleService
Phase 4:  Orchestrator::init() → Orchestrator::run()
```

### GoApp::run_interactive()

Same logic as current `run_interactive()` in `main.cpp`.  Conditional
init calls become board accessor calls:

```cpp
// Current:  if (!ctx.config_store) init_core(ctx);
// Becomes:  _board.init_core();  // idempotent
```

Because GoBoard init methods are idempotent, the conditional checks
(`if (!ctx.config_store)`, `if (!ctx.sensor_manager)`, etc.) are
replaced by direct calls.  Already-initialized services return the
cached instance.

### Pure utility functions

Co-located in `go_app.h/cpp` (not a separate module):

```cpp
// select_boot_path — replaces inline logic in app_main
BootPath select_boot_path(WakeCause cause, const RtcAppState &state) {
    if (cause == WakeCause::Timer) {
        if (PowerService::is_fast_path_wake(cause, state)) {
            return BootPath::FastPath;
        }
    }
    if (cause == WakeCause::Button && state.mode == OperatingMode::Offline) {
        return BootPath::ButtonWake;
    }
    return BootPath::Interactive;
}

// is_gps_active_at_boot — replaces 3 inline copies
bool is_gps_active_at_boot(const GoSettings &settings,
                            const RtcAppState &state) {
    return (settings.gps_mode == GpsMode::AlwaysOn) ||
           (settings.gps_mode == GpsMode::OnWhenTracking &&
            state.tracking_active);
}
```

`measures_to_ago()`, `build_fast_path_display()`, `build_wake_values()`
move from `main.cpp` unchanged.

## main.cpp After Refactor

```cpp
#include "go_app.h"
#include "go_hardware_board.h"

extern "C" void app_main() {
    GoHardwareBoard board;
    GoApp app(board);
    app.run();
}
```

## Test Strategy

### Test pattern: link-time stubs + MockBoard

Follows the established orchestrator test pattern:

1. `go_app.cpp` (code under test) compiles against real headers
2. `go_app_stubs.cpp` replaces product service `.cpp` files at link time
3. `MockBoard` implements `GoBoard` and returns stub service instances
4. `GoAppTestAccess` friend class exposes `execute_fast_path()` and
   other private members for assertion

### go_app_stubs.cpp

Provides the same kind of observable stubs as
`go_orchestrator_stubs.cpp`:

- `SensorProducer` — stub (start/stop tracked)
- `GpsService` — stub (start/stop/idle tracked)
- `InputService` — stub (start/stop tracked)
- `DisplayService` — uses the existing `#ifdef TEST_HOST` stub from
  `go_display.h`
- `StorageService` — stub (cache_measurement, route ops tracked in
  `test_spy`)
- `PowerService` — stub (poll_bms, decide_sleep, enter_sleep tracked)
- `BleService` — stub (init/deinit tracked)
- `Orchestrator` — stub (init/run tracked, run returns immediately)
- `load_rtc_app_state()` — returns configurable `test_spy::rtc_state`
- `load_rtc_display_snapshot()` — returns configurable snapshot
- `gps_read_once()` — returns configurable GPS data

Note: some stubs can be shared with (or copied from)
`go_orchestrator_stubs.cpp`.  The exact sharing strategy is an
implementation detail — the spec does not mandate deduplication.

### MockBoard

```cpp
class MockBoard : public GoBoard {
public:
    // Init tracking
    bool nvs_init_called = false;
    bool buses_init_called = false;
    bool spi_init_called = false;
    bool bms_init_called = false;
    bool core_init_called = false;
    bool sensors_warm_arg = false;

    // ISR
    volatile bool *isr_flag = nullptr;
    int isr_pin = -1;
    bool isr_installed = false;
    bool isr_removed = false;

    // Init methods — track calls, no-op
    void init_nvs() override { nvs_init_called = true; }
    void init_buses() override { buses_init_called = true; }
    void init_spi() override { spi_init_called = true; }
    void init_bms() override { bms_init_called = true; }
    void init_core() override {
        core_init_called = true;
        init_nvs(); init_buses(); init_spi(); init_bms();
    }

    // Service accessors — return test stubs
    ConfigStore &config_store() override { return _config_store; }
    GoSettings load_settings() override { return settings; }
    BmsDevice &bms() override { return _bms; }
    SensorManager &sensors(bool warm) override {
        sensors_warm_arg = warm;
        return *_sensor_manager;
    }
    StorageService &storage() override { return _storage; }
    DisplayService &display() override { return _display; }
    PowerService &power() override { return _power; }

    GpsDriver *new_gps_driver() override { /* ... */ }
    CapTouchSensor *new_touch_sensor() override { /* ... */ }

    std::string serial_number() override { return "test-serial"; }
    const char *firmware_version() override { return "0.0.0-test"; }
    const gpio::Hal &gpio_hal() override { return _gpio_hal; }
    void release_gpio_holds() override { /* no-op */ }
    void ulp_stop() override { /* no-op */ }
    void ulp_start() override { /* no-op */ }

    void install_button_isr(int pin, volatile bool *flag) override {
        isr_pin = pin;
        isr_flag = flag;
        isr_installed = true;
    }
    void remove_button_isr(int pin) override {
        isr_removed = true;
    }

    // --- Test helpers ---

    /// Simulate button press (as if ISR fired)
    void press_button() {
        if (isr_flag) *isr_flag = true;
    }

    // Configurable test state
    GoSettings settings{};

    // Stub service instances (constructed in MockBoard constructor
    // or test fixture)
    // ...
};
```

### GoAppTestAccess

```cpp
class GoAppTestAccess {
public:
    explicit GoAppTestAccess(GoApp &app) : _app(app) {}

    GoApp::FastPathResult execute_fast_path(
        const RtcAppState &state,
        const volatile bool &button,
        const RtcDisplaySnapshot *snapshot = nullptr,
        bool snapshot_valid = false) {
        return _app.execute_fast_path(state, button, snapshot, snapshot_valid);
    }

private:
    GoApp &_app;
};
```

### Test scenarios

Tests for `execute_fast_path()`:

| Scenario | Setup | Assert |
|---|---|---|
| Warm sensors skip warmup, measure, sleep | `state.sensors_warm = true`, long interval | Outcome::Sleep, has_measures, `sensors(true)` called |
| Cold sensors full warmup, measure, sleep | `state.sensors_warm = false`, long interval | Outcome::Sleep, has_measures |
| Button during warmup → promote unlocked | Set `*isr_flag = true` during warmup_step | Outcome::Promote, unlock, suppress_wake, no measures |
| Button during measurement → promote | Set flag after warmup | Outcome::Promote, has_measures (partial) |
| Button during GPS → promote | Set flag during gps_read_once | Outcome::Promote, has_measures, no GPS |
| Sleep too short → promote locked | Short interval, decide_sleep returns None | Outcome::Promote, locked, display_painted |
| Tracking + GPS active → route point stored | tracking_active + AlwaysOn | storage start_route + append called |
| Tracking + GPS off → no GPS read | tracking_active + AlwaysOff | new_gps_driver not called |
| No tracking → no route | tracking_active = false | storage start_route not called |

Tests for pure functions:

| Function | Key scenarios |
|---|---|
| `select_boot_path` | Timer+Locked→FastPath, Timer+Unlocked→Interactive, Button+Offline→ButtonWake, Button+Portable→Interactive, PowerOn→Interactive |
| `measures_to_ago` | Valid fields mapped, invalid fields preserved |
| `build_fast_path_display` | Valid sensors → values populated, invalid → sentinels, settings (fahrenheit, usaqi) applied |
| `build_wake_values` | Snapshot valid → values seeded, snapshot invalid → defaults, always unlocked + "Unlocked" snackbar |
| `is_gps_active_at_boot` | AlwaysOn → true, AlwaysOff → false, OnWhenTracking + tracking → true, OnWhenTracking + idle → false |

### CMake test target

Add to `products/go/tests/CMakeLists.txt`:

```cmake
# ---------------------------------------------------------------------------
# go_app tests
# ---------------------------------------------------------------------------

add_library(go_app_test_support
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_app.cpp"
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_settings.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/common.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
    go_app_stubs.cpp
)

target_include_directories(go_app_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/products/go/main"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-config/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms/types"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-nand-storage/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-touch/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gpio/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-serial/include"
)

target_compile_definitions(go_app_test_support PUBLIC
    TEST_HOST
    CONFIG_PAYLOAD_CACHE_TYPE_AGO=1
    CONFIG_PAYLOAD_CACHE_MAX_SIZE=16
    CONFIG_SENSOR_WARMUP_DURATION_MS=10000
    CONFIG_SENSOR_WARMUP_INTERVAL_MS=1000
)

add_executable(go_app_tests
    go_app.tests.cpp
)

target_link_libraries(go_app_tests PRIVATE
    go_app_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(go_app_tests)
```

## Migration Plan

This is a refactor — no logic changes.  Every line of application logic
moves to GoApp or GoHardwareBoard unchanged.  The hardware init code
moves to GoHardwareBoard unchanged.

### Steps

1. **Create `go_board.h`** — the abstract interface (new file, no deps)

2. **Create `go_hardware_board.h/cpp`** — move all `init_*` functions
   and `BootContext` logic from `main.cpp`.  Map each `init_*` to a
   method.  Add idempotency guards.  Keep `board_config.h` includes
   confined to this file.

3. **Create `go_app.h/cpp`** — move boot paths (`run_fast_path`,
   `run_button_wake_path`, `run_interactive`) and pure functions from
   `main.cpp`.  Replace `ctx.` member access with `_board.` calls.
   Extract `execute_fast_path()` from `run_fast_path()`.

4. **Rewrite `main.cpp`** — thin shell (construct board, construct app,
   call run).

5. **Update `CMakeLists.txt`** — add `go_hardware_board.cpp` and
   `go_app.cpp` to `SRCS`.

6. **Verify firmware build** — `idf.py -C products/go build` succeeds
   with no logic changes.

7. **Create test files** — `go_app_stubs.cpp`, `go_app.tests.cpp`.
   Add CMake target.

8. **Verify tests** — `ctest --test-dir tests/build --output-on-failure`
   passes (existing tests unchanged, new tests pass).

### Risk mitigation

- No logic changes — each function moves verbatim, with `ctx.foo`
  replaced by `_board.foo()`.
- Existing orchestrator tests are completely untouched.
- `board_config.h` pins/constants are used by GoHardwareBoard only,
  same as today.
- The firmware binary output should be functionally identical (same
  init sequence, same service construction, same orchestrator).

## Interaction with Existing Components

### BootHandoff and Orchestrator

`BootHandoff` (defined in `go_types.h`) is unchanged.  GoApp constructs
it the same way as current `main.cpp` and passes it to
`Orchestrator::init()`.  The Orchestrator does not know about GoApp or
GoBoard.

### RTC state persistence

`load_rtc_app_state()` and `PowerService::save_state()` /
`PowerService::load_state()` are unchanged.  GoApp calls them the same
way as current `main.cpp`.  In tests, these are stubbed at link time
(same pattern as orchestrator tests).

### DisplayService TEST_HOST stub

The existing `#ifdef TEST_HOST` class replacement in `go_display.h` is
reused as-is.  No changes to the display header or its test stub.

## Documentation Updates

After implementation, update:

- `products/go/ARCHITECTURE.md` — §3 and §7.4 to reference GoApp and
  GoBoard instead of `main.cpp` boot paths
- `products/go/docs/hardware_init.md` — reference GoHardwareBoard
  instead of `main.cpp` init helpers (if this doc exists)

## Acceptance Criteria

- `main.cpp` is ≤15 lines with no application logic
- All boot path selection logic is host-testable via `select_boot_path()`
- All fast-path control flow is host-testable via `execute_fast_path()`
  through `GoAppTestAccess`
- All data transforms are host-testable as pure functions
- `is_gps_active_at_boot()` replaces the 3 duplicated inline checks
- GoBoard init methods are idempotent
- GoHardwareBoard contains all ESP-IDF/hardware calls — GoApp has none
- Firmware build output is functionally identical
- Existing tests (orchestrator, power, storage, etc.) pass unchanged
- New `go_app_tests` test target builds and passes
