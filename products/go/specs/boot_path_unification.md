# Boot Path Unification — Implementation Spec

Unify AGo's three boot paths (timer-wake fast path, button-wake path, full
boot) to eliminate duplicated initialization, fix a latent crash on fast-path
promotion, and enable interruptibility during the fast path.

The approach is:

1. **Shared init helpers** — extract the identically-duplicated init blocks
   (NVS, GPIO, I2C, SPI, BMS, sensors, storage) into composable file-local
   helpers that write results into a `BootContext` struct.
2. **`BootContext`** — lightweight struct that accumulates initialized hardware
   handles and driver pointers as boot progresses. Passed to the interactive
   boot on fast-path promotion so already-initialized resources are reused,
   not double-initialized.
3. **`BootHandoff`** — structured description of what boot has already done,
   replacing the ad-hoc `(already_painted, snapshot*)` parameters on
   `Orchestrator::init()`.
4. **Fast-path interruptibility** — ISR-based button detection during blocking
   warmup/measurement/GPS operations, with clean promotion to interactive
   boot.
5. **`warmup_step()` + `warmup()`** — split `SensorManager::warmup_sensor()`
   into a single-cycle primitive and a blocking convenience loop so the fast
   path can drive warmup timing with abort checks between iterations.

## Background

Today, `products/go/main/main.cpp` has three boot path functions:

- **`run_fast_path()`** — timer wake when locked in Offline mode. Minimal
  init, blocking warmup, one-shot measurement, display update, sleep. No
  event loop, no producer tasks. Goal: measure and sleep as fast as possible
  to conserve battery.
- **`run_button_wake_path()`** — button wake in Offline mode. Four-phase
  boot: early RTC-backed display paint (~10 ms), parallel non-SPI init
  (~300 ms), NAND init (blocks on SPI until display refresh finishes ~3 s),
  then Orchestrator with `already_painted=true`.
- **`run_full_boot()`** — fresh power-on, button wake in non-Offline modes,
  and fallthrough from fast path. Full sequential init, empty dashboard
  display, then Orchestrator.

These paths duplicate ~120 lines of identical init code (NVS, settings,
GPIO, I2C, SPI, BMS, sensor drivers, storage). Sensor driver construction
alone is ~40 lines copied verbatim three times.

### Latent crash on fast-path promotion

`run_fast_path()` can return to `app_main()` when `decide_sleep()` returns
`SleepType::None` (measurement interval too short for deep sleep overhead).
The comment in `app_main()` says "Never returns" but the fast path code
explicitly documents this return path (lines 306-309). When it returns,
`app_main()` falls through to `run_full_boot()`, which calls `init_nvs()`,
`i2c_new_master_bus()`, `spi_bus_initialize()`, etc. on already-initialized
hardware. ESP-IDF does not allow double initialization of these resources —
the firmware panics.

This crash is the primary correctness motivation for this work.

### Fast-path interruptibility

During `run_fast_path()`, there is no active input runtime. The system
executes a blocking sequence: warmup (~10 s), one-shot measurement,
optional GPS read (~2 s timeout), storage, display update (~3 s). A user
pressing the power button during this window gets no response. This is a
near-term product requirement: the device must be able to detect a button
press during fast path and promote to interactive mode.

## Files

| File | Change |
|---|---|
| `components/airgradient-sensors/services/sensor_manager.h` | Add `warmup_step()`, rename `warmup_sensor()` to `warmup()` |
| `components/airgradient-sensors/services/sensor_manager.cpp` | Extract single-cycle body into `warmup_step()`, rewrite `warmup()` to call it in a loop |
| `components/airgradient-sensors/tests/sensor_manager.tests.cpp` | Rename `warmup_sensor()` calls to `warmup()`, add `warmup_step()` test section |
| `products/go/main/go_types.h` | Add `BootHandoff` struct |
| `products/go/main/go_orchestrator.h` | Change `init()` signature to accept `const BootHandoff &` |
| `products/go/main/go_orchestrator.cpp` | Adapt `init()` for `BootHandoff` fields; add Timer-wake promotion handling; generalize RTC state restoration |
| `products/go/main/main.cpp` | Add `BootContext` struct; extract shared init helpers; refactor all three boot paths; add `run_interactive()`; add ISR-based button detection in fast path |
| `products/go/main/gps/gps_service.h` | Add `const volatile bool &abort` parameter to `gps_read_once()` |
| `products/go/main/gps/gps_service.cpp` | Check `abort` flag in `gps_read_once()` polling loop |
| `products/go/tests/go_orchestrator.tests.cpp` | Update all `init()` calls to use `BootHandoff`; add promotion test cases |

**Not touched:** `go_orchestrator.h` Services struct, `go_orchestrator.cpp`
`run()`/`dispatch()`/`try_enter_sleep()`/`prepare_for_sleep()`, any service
interface other than SensorManager warmup rename and `gps_read_once()` abort
parameter.

## Dependencies

No new dependencies. All changes use existing ESP-IDF GPIO ISR APIs and
existing project abstractions. No `idf_component.yml` changes.

## Design Decisions

### BootContext as a resource accumulator, not a state machine

Boot is a forward-only initialization pipeline, not a state machine. You
never go back to "init GPIO" from "init sensors." There are no runtime
transitions between boot states. A formal FSM (state enums, transition
tables, `tick()` loop) does not match the actual control flow and adds
abstraction overhead for a linear sequence.

`BootContext` is a plain struct that accumulates initialized hardware handles
and driver pointers as boot progresses. Each init helper writes its results
into the context. On fast-path promotion, the partially-filled context is
passed to `run_interactive()` which completes the remaining init. No
idempotency logic — each init helper is called exactly once per boot,
determined by the path.

`BootContext` is file-local in `main.cpp`. It is not a public interface.
Boot logic remains procedural functions in `main.cpp`.

### BootHandoff replaces ad-hoc Orchestrator parameters

The current `Orchestrator::init()` takes `(WakeCause, bool already_painted,
const RtcDisplaySnapshot*)`. The `already_painted` boolean encodes a
multi-faceted boot history that is becoming insufficient:

- Button-wake: display painted, wake press suppressed, unlocked, snapshot
  available
- Fast-path promotion with button press: display may or may not be painted,
  wake press should be suppressed, unlocked, measurement may be available
- Fast-path promotion without button (sleep too short): display painted with
  locked content, locked, measurement completed

A `BootHandoff` struct makes each dimension explicit and independently
settable. The Orchestrator's `init()` logic is driven by handoff fields
rather than wake-cause-specific branches.

### Shared init helpers are file-local, not a class

The extracted init helpers (`init_core()`, `init_sensors()`, etc.) are
static functions in `main.cpp` that take `BootContext &`. They are not
methods of a `BootController` class. Boot-specific product init logic
belongs in the product's `main.cpp`, not in a shared component. There is
no reuse case for these helpers outside AGo.

### ISR-based button detection during fast path

Polling `gpio_get_level()` between blocking operations risks missing a
quick button press during an I2C transaction or sensor read. An ISR on
`PIN_BUTTON_POWER` (falling edge) sets a `volatile bool` flag that persists
until checked. The ISR does no allocation, logging, or blocking — it only
sets the flag. This is safe for ISR context.

The ISR is installed at the start of `run_fast_path()` and removed before
promotion (before `InputService` is constructed, which configures the same
GPIO for its own interrupt-based debounce). Installing the GPIO ISR service
(`gpio_install_isr_service()`) is idempotent in ESP-IDF — safe to call if
already installed. Removal before `InputService` construction avoids
double-handler conflicts.

### warmup_step() and warmup() split

`SensorManager::warmup_sensor()` is a blocking loop of 10 iterations × 1 s.
The fast path needs to check for button presses between iterations. Rather
than adding a callback parameter to the existing function, splitting the API
is simpler and more explicit:

- `warmup_step()` — single cycle: TVOC/NOx conditioning + PM discard read.
  No delay. Caller controls timing.
- `warmup()` — convenience: full blocking loop calling `warmup_step()` with
  paced delays. Functionally identical to the current `warmup_sensor()`.

`warmup_step()` returns immediately if no TVOC/NOx or PM sensors are present
(same guard as the current function). `warmup()` calls `warmup_step()`
internally and handles the delay/timing logic.

`warmup_sensor()` is removed. The one existing call site in `main.cpp` moves
to the interruptible loop pattern.

### gps_read_once abort parameter

`gps_read_once()` is a synchronous polling loop with a 2 s timeout. To make
it interruptible, a `const volatile bool &abort` parameter is added. The
polling loop checks `abort` alongside the timeout deadline:

```cpp
while (RTOS::get_time_ms() < deadline_ms && !abort) { ... }
```

The existing call with default `abort = false` (a static const) preserves
current behavior for any future callers that don't need abort support.

The volatile reference avoids copying a flag that may be set asynchronously
by an ISR.

### Promotion replaces fallthrough

The current fast-path-to-full-boot fallthrough (returning from
`run_fast_path()` to `app_main()` which falls through to `run_full_boot()`)
is the latent crash. This is replaced with explicit promotion:
`run_fast_path()` calls `run_interactive()` directly, passing its
`BootContext` and a `BootHandoff` describing what has already been done.
`run_fast_path()` never returns to `app_main()`.

### run_interactive() as unified interactive entry

`run_full_boot()` is replaced by `run_interactive()` which accepts a
`BootContext &` and `BootHandoff`. For fresh boot, the context is empty and
all init runs. For fast-path promotion, the context is partially filled and
only the remaining init runs. This eliminates the duplicate init code between
`run_full_boot()` and the promotion path.

`run_button_wake_path()` is NOT merged into `run_interactive()`. The
button-wake path has a fundamentally different init ordering (SPI first for
early paint, then non-SPI init in parallel with display refresh, then NAND
init blocks on SPI bus serialization). Forcing this into a generic
`run_interactive()` would lose the intentional parallelism. The button-wake
path uses the shared init helpers but keeps its own sequencing.

### Display tracking in BootContext

`BootContext` tracks `display` as a pointer (initially null). If non-null,
the display hardware is initialized and `display_service.update()` can be
called without a prior `init()`. `run_interactive()` skips display
construction and `init()` when `ctx.display` is non-null.

Whether the orchestrator should repaint on entry is controlled by
`handoff.display_painted`:

- `display_painted = true`: display shows correct content for the current
  state. Orchestrator skips initial `update_display()`.
- `display_painted = false`: display either hasn't been initialized (fresh
  boot, `run_interactive()` handles `init()`) or shows stale/wrong content
  (e.g., fast-path painted locked dashboard but user pressed button to
  unlock). Orchestrator triggers `update_display()` via `unlock()` or
  initial measurement request.

### Orchestrator RTC state restoration generalized

The current `init()` only restores RTC state (`behavior`, `gps_enabled`,
`tracking_active`, `tracking_session_id`) for `WakeCause::Button`. With
fast-path promotion, Timer-wake also needs RTC restoration (the device was
in a sleep cycle with persisted state).

The generalized rule: restore RTC state when `cause != WakeCause::PowerOn`.
Both Timer and Button wakes come from deep sleep and have valid RTC state.
PowerOn is a fresh boot with no prior state.

### Orchestrator first_measurement_done for promotion

When `handoff.measurement_completed` is true (fast path already took a
measurement), the Orchestrator sets `_first_measurement_done = true` at
init time. This allows the sleep-too-short promotion case to immediately
attempt sleep on the next event loop iteration (the device is locked and a
measurement is done). Without this, the Orchestrator would never try to
sleep because `_first_measurement_done` stays false until a
`SensorDataReady` event arrives.

## New Types

### BootContext (file-local in main.cpp)

```cpp
/// Tracks hardware resources initialized during boot. Populated
/// incrementally by init helpers. Passed to run_interactive() on
/// fast-path promotion to avoid double-initialization.
///
/// All pointers are heap-allocated and never freed (the Orchestrator's
/// run() never returns). On fast-path sleep, enter_sleep() reboots the
/// CPU and the heap is reclaimed.
struct BootContext {
    // Buses
    i2c_master_bus_handle_t i2c_bus = nullptr;
    bool spi_ready = false;

    // Settings
    NvsConfigStore *config_store = nullptr;
    GoSettings settings{};

    // Drivers
    BQ25629Bms *bms = nullptr;
    SensorManager *sensor_manager = nullptr;

    // Storage
    PayloadCache *cache = nullptr;
    StorageService *storage = nullptr;

    // Display
    DisplayService *display = nullptr;

    // Power
    PowerService *power_service = nullptr;
};
```

### BootHandoff (in go_types.h)

```cpp
/// Boot-to-runtime handoff. Describes what boot has already done so the
/// Orchestrator can skip redundant work and pick up where boot left off.
///
/// Default-initialized values represent a fresh power-on boot where
/// nothing has been done yet.
struct BootHandoff {
    /// Display already shows a valid frame for the current state.
    /// When true, init() skips update_display() and sets state directly.
    bool display_painted = false;

    /// A measurement was already completed during boot (fast-path).
    /// When true, the Orchestrator sets _first_measurement_done and
    /// skips the initial measurement request.
    bool measurement_completed = false;

    /// Suppress the first ButtonPower short-press event. Used when the
    /// wake press that triggered boot should not toggle lock state.
    bool suppress_wake_press = false;

    /// Initial lock state. Boot paths that unlock early (button wake,
    /// fast-path promotion on button press) set this to Unlocked.
    LockState initial_lock_state = LockState::Locked;

    /// Optional RTC display snapshot for seeding stale display values.
    /// Used by button-wake path to avoid dashes before fresh data arrives.
    const RtcDisplaySnapshot *display_snapshot = nullptr;

    /// Optional measurement from fast-path boot (promotion case).
    /// When non-null, the Orchestrator seeds _cached_measures.
    const MeasuresAGo *fast_path_measures = nullptr;
};
```

## API Changes

### sensor_manager.h

Remove `warmup_sensor()`. Add:

```cpp
/// Run a single warmup cycle: TVOC/NOx conditioning + PM discard read.
///
/// Does NOT pace itself — caller controls timing between calls.
/// Returns immediately if no TVOC/NOx or PM sensors are present.
void warmup_step();

/// Run a full blocking warmup loop.
///
/// Calls warmup_step() in a paced loop for CONFIG_SENSOR_WARMUP_DURATION_MS.
/// Each iteration is paced to CONFIG_SENSOR_WARMUP_INTERVAL_MS.
/// Returns immediately if no TVOC/NOx or PM sensors are present.
void warmup();
```

### sensor_manager.cpp — warmup_step()

```cpp
void SensorManager::warmup_step() {
  if (!_sensors.tvoc_nox && !_sensors.pms_a && !_sensors.pms_b) {
    return;
  }

  if (_sensors.tvoc_nox) {
    if (!_sensors.tvoc_nox->run_conditioning()) {
      AG_LOGW(TAG, "TVOC/NOx conditioning failed");
    }
  }

  PMData discard;
  if (_sensors.pms_a) {
    (void)_sensors.pms_a->read(discard);
  }
  if (_sensors.pms_b) {
    (void)_sensors.pms_b->read(discard);
  }
}
```

### sensor_manager.cpp — warmup()

```cpp
void SensorManager::warmup() {
  if (!_sensors.tvoc_nox && !_sensors.pms_a && !_sensors.pms_b) {
    return;
  }

  const int iterations = CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS;
  AG_LOGI(TAG, "warmup: %d iterations (%d ms interval)", iterations,
          CONFIG_SENSOR_WARMUP_INTERVAL_MS);
  for (int i = 0; i < iterations; i++) {
    AG_LOGI(TAG, "warmup: iteration %d/%d", i + 1, iterations);
    uint64_t start_time_ms = RTOS::get_time_ms();

    warmup_step();

    uint64_t elapsed_time_ms = RTOS::get_time_ms() - start_time_ms;
    if (elapsed_time_ms < CONFIG_SENSOR_WARMUP_INTERVAL_MS) {
      uint32_t delay_ms =
          static_cast<uint32_t>(CONFIG_SENSOR_WARMUP_INTERVAL_MS - elapsed_time_ms);
      RTOS::delay_ms(delay_ms);
    }
  }
}
```

### go_orchestrator.h — init() signature

Replace:

```cpp
void init(WakeCause cause, bool already_painted = false,
          const RtcDisplaySnapshot *snapshot = nullptr);
```

With:

```cpp
/// Set initial state from boot context and perform first-boot actions.
/// Call once before run().
///
/// @param cause   Wake cause from PowerService::get_wake_cause().
/// @param handoff Boot-to-runtime handoff describing what boot has
///                already done. Default is a fresh power-on.
void init(WakeCause cause, const BootHandoff &handoff = {});
```

### go_orchestrator.cpp — init() body

```cpp
void Orchestrator::init(WakeCause cause, const BootHandoff &handoff) {
  AG_LOGI(TAG, "init: wake_cause=%d display_painted=%d measurement_done=%d "
          "lock=%d",
          static_cast<int>(cause), static_cast<int>(handoff.display_painted),
          static_cast<int>(handoff.measurement_completed),
          static_cast<int>(handoff.initial_lock_state));

  // Mode always comes from persisted settings (NVS) — single source of truth
  _mode = _settings.operating_mode;

  // --- Restore RTC state for wake-from-sleep cases ---
  if (cause != WakeCause::PowerOn) {
    RtcAppState state = _svc.power_service.load_state();
    _behavior = state.behavior;
    _gps_enabled = state.gps_enabled;
    _tracking_active = state.tracking_active;
    _tracking_session_id = state.tracking_session_id;
  }

  // --- Apply initial lock state ---
  if (handoff.initial_lock_state == LockState::Unlocked) {
    if (handoff.display_painted) {
      // Display already shows unlocked UI — set state directly.
      // Do NOT call unlock() which would trigger update_display().
      _lock_state = LockState::Unlocked;
      _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.show_snackbar("Unlocked");
      uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.clear_expired_snackbar(now_ms);
      _snackbar_refresh_deadline_ms = now_ms + SNACKBAR_DURATION_MS + 200;
    } else {
      // Display not yet showing unlocked UI — unlock triggers
      // update_display() which will paint the unlocked frame.
      unlock();
    }
  }

  // --- Seed cached measures from boot ---
  if (handoff.display_snapshot != nullptr) {
    _cached_measures.co2.co2 = handoff.display_snapshot->co2_ppm;
    _cached_measures.pm_a.pm_25 = handoff.display_snapshot->pm25_ugm3;
    _cached_measures.temp_hum_a.temperature = handoff.display_snapshot->temperature_c;
    _cached_measures.temp_hum_a.humidity = handoff.display_snapshot->humidity_pct;
    _cached_measures.tvoc_nox.tvoc_index = handoff.display_snapshot->tvoc_index;
    _cached_measures.tvoc_nox.nox_index = handoff.display_snapshot->nox_index;
    _cached_measures.pressure.pressure = handoff.display_snapshot->pressure_hpa;
    _cached_measures.pressure.altitude = handoff.display_snapshot->altitude_m;
  } else if (handoff.fast_path_measures != nullptr) {
    _cached_measures = *handoff.fast_path_measures;
  }

  // --- Mark first measurement done if boot already measured ---
  if (handoff.measurement_completed) {
    _first_measurement_done = true;
  }

  // --- Resume route if tracking was active before sleep ---
  if (_tracking_active) {
    _svc.storage_service.start_route(_tracking_session_id);
  }

  // --- Common tail ---
  _svc.ui_manager.sync_settings(_settings);

  if (!handoff.measurement_completed) {
    _svc.sensor_producer.request_measurement(1, SensorGroup::All);
  }

  _latest_power = _svc.power_service.poll_bms();

  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_measurement_ms = now;
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now;
  _last_ext_wdt_ms = now;
  if (handoff.initial_lock_state != LockState::Unlocked || !handoff.display_painted) {
    _last_input_ms = now;
  }

  init_ble_if_portable();
}
```

### gps_service.h — gps_read_once() abort parameter

Replace:

```cpp
GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms);
```

With:

```cpp
/// Synchronous one-shot GPS read. Blocks until a valid fix is parsed,
/// timeout_ms expires, or abort becomes true. For use in the fast-path
/// timer-wake boot path only — does not require a running RTOS task.
///
/// @param abort  When true, the polling loop exits early. Intended for
///               ISR-driven abort flags (volatile bool set by ISR).
///               Default: a static false constant (no abort support).
GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms,
                      const volatile bool &abort = false);
```

Note: the default argument `false` binds to a `const volatile bool &` by
creating a temporary. This is valid in C++ because `const` references can
bind to temporaries.

### gps_service.cpp — gps_read_once() with abort

```cpp
GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms,
                      const volatile bool &abort) {
  driver.begin(baud_rate);
  const uint64_t deadline_ms = RTOS::get_time_ms() + timeout_ms;
  while (RTOS::get_time_ms() < deadline_ms && !abort) {
    if (driver.read() && driver.has_valid_fix()) {
      break;
    }
    RTOS::delay_ms(10);
  }
  const GpsData data = driver.get_data();
  driver.end();
  return data;
}
```

## Boot Path Refactoring

### Shared init helpers (file-local in main.cpp)

These replace the duplicated init blocks across the three boot paths. Each
helper writes its results into `BootContext &ctx`.

```cpp
/// Initialize NVS flash, construct config store, load settings.
static void init_settings(BootContext &ctx) {
  init_nvs();
  ctx.config_store = new NvsConfigStore("go");
  ctx.settings = load_go_settings(*ctx.config_store);
  print_settings(ctx.settings);
}

/// Initialize GPIO power enables + I2C bus + settling delays.
static void init_buses(BootContext &ctx) {
  init_gpio();
  RTOS::delay_ms(100);
  ctx.i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);
}

/// Initialize SPI bus(es).
static void init_spi(BootContext &ctx) {
  init_spi_buses();
  ctx.spi_ready = true;
}

/// Initialize BMS (requires I2C).
static void init_bms_driver(BootContext &ctx) {
  ctx.bms = init_bms(ctx.i2c_bus);
  if (!ctx.bms->init()) {
    AG_LOGE(TAG, "BMS init failed");
  }
}

/// Construct and init all sensor drivers, build SensorManager.
static void init_sensors(BootContext &ctx) {
  auto *sgp41 = new SGP41(ctx.i2c_bus, I2C_ADDR_SGP41);
  auto *sps30 = new SPS30(ctx.i2c_bus);
  auto *dps368 = new DPS368(ctx.i2c_bus, I2C_ADDR_DPS368);

  Sensors sensors{};
  sensors.co2 = init_co2_sensor(ctx.i2c_bus);

  if (sgp41->init()) {
    sensors.tvoc_nox = sgp41;
  } else {
    AG_LOGE(TAG, "SGP41 init failed");
  }
  if (sps30->init()) {
    sensors.pms_a = sps30;
  } else {
    AG_LOGE(TAG, "SPS30 init failed");
  }
  if (dps368->init()) {
    sensors.pressure = dps368;
  } else {
    AG_LOGE(TAG, "DPS368 init failed");
  }

  sensors.temp_hum_a_fallback.priority[0] = TempHumSource::CO2;
  sensors.temp_hum_a_fallback.priority[1] = TempHumSource::PRESSURE;
  sensors.temp_hum_a_fallback.count = 2;

  ctx.sensor_manager = new SensorManager(sensors);
}

/// Initialize RTC payload cache + NAND flash + StorageService.
static void init_storage(BootContext &ctx) {
  auto *rtc_storage = new RtcPayloadCacheStorage();
  ctx.cache = new PayloadCache(*rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

  SpiNandStorage::Config nand_config{};
  nand_config.spi_host = SPI_HOST;
  nand_config.cs_pin = PIN_NAND_CS;
  auto *nand = new SpiNandStorage(nand_config);

  ctx.storage = new StorageService(*ctx.cache, *nand);
  ctx.storage->restore_cache();
  if (!ctx.storage->init()) {
    AG_LOGE(TAG, "NAND storage init failed");
  }
}

/// Initialize PowerService + external watchdog.
static void init_power(BootContext &ctx) {
  ctx.power_service =
      new PowerService(*ctx.bms, gpio::native::hal,
                       {
                           .pin_wake_button_power = PIN_BUTTON_POWER,
                           .pin_wake_button_boot = -1,
                           .pin_ext_wdt = PIN_EXT_WDT,
                       });
  ctx.power_service->init_ext_watchdog();
  ctx.power_service->reset_ext_watchdog();
}

/// Initialize display service (construction only, no init/paint).
static void init_display(BootContext &ctx) {
  ctx.display = new DisplayService({
      .spi_host = SPI_HOST,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });
}
```

The existing `init_nvs()`, `init_gpio()`, `init_i2c_bus()`, `init_spi_buses()`,
`init_bms()`, `init_co2_sensor()` free functions remain unchanged. The new
helpers compose them and write results into `BootContext`.

`init_core()` is a convenience that chains the common early init:

```cpp
/// Common early init: settings + buses + SPI + BMS.
static void init_core(BootContext &ctx) {
  init_settings(ctx);
  init_buses(ctx);
  init_spi(ctx);
  init_bms_driver(ctx);
}

/// Same as init_core but skips SPI (already initialized for early paint).
static void init_core_no_spi(BootContext &ctx) {
  init_settings(ctx);
  init_buses(ctx);
  // SPI already initialized — ctx.spi_ready must be true.
  init_bms_driver(ctx);
}
```

### app_main() — simplified boot path selection

```cpp
extern "C" void app_main() {
  RTOS::delay_ms(100);
  WakeCause cause = PowerService::get_wake_cause();

  if (cause == WakeCause::Timer) {
    RtcAppState state = load_rtc_app_state();
    if (PowerService::is_fast_path_wake(cause, state)) {
      run_fast_path(state);
      // Never returns — sleeps or promotes to interactive.
    }
  }

  if (cause == WakeCause::Button) {
    RtcAppState state = load_rtc_app_state();
    if (state.mode == OperatingMode::Offline) {
      run_button_wake_path(state);
      // Never returns.
    }
  }

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial_number.c_str());

  BootContext ctx;
  run_interactive(cause, ctx, {});
  // Never returns.
}
```

Key change: `run_fast_path()` never returns to `app_main()`. Either it
sleeps or it calls `run_interactive()` internally. The crash is fixed.

### run_fast_path() — interruptible, with promotion

```cpp
static void run_fast_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_fast_path: entering fast-path boot");
  const uint32_t boot_time_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // --- ISR setup for button detection ---
  static volatile bool s_button_pressed = false;
  s_button_pressed = false;
  gpio_install_isr_service(0);  // idempotent
  gpio_set_intr_type(PIN_BUTTON_POWER, GPIO_INTR_NEGEDGE);
  gpio_isr_handler_add(PIN_BUTTON_POWER, [](void *arg) {
    *static_cast<volatile bool *>(arg) = true;
  }, const_cast<volatile bool *>(&s_button_pressed));

  // --- Core init ---
  BootContext ctx;
  init_core(ctx);
  init_sensors(ctx);

  bool promote = false;

  // --- Interruptible warmup ---
  const int warmup_iters =
      CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS;
  for (int i = 0; i < warmup_iters && !promote; i++) {
    uint64_t start = RTOS::get_time_ms();
    ctx.sensor_manager->warmup_step();

    if (s_button_pressed) { promote = true; break; }

    uint64_t elapsed = RTOS::get_time_ms() - start;
    if (elapsed < CONFIG_SENSOR_WARMUP_INTERVAL_MS) {
      RTOS::delay_ms(
          static_cast<uint32_t>(CONFIG_SENSOR_WARMUP_INTERVAL_MS - elapsed));
    }
    if (s_button_pressed) { promote = true; break; }
  }

  // --- One-shot measurement (skip if promoting) ---
  MeasuresAGo ago{};
  bool has_measures = false;
  if (!promote) {
    Measures measures =
        ctx.sensor_manager->start_measures(1, SensorGroup::All);
    // TODO: raw-to-index placeholder (remove when algorithm is applied)
    measures.tvoc_nox.tvoc_index = measures.tvoc_nox.tvoc_raw;
    measures.tvoc_nox.nox_index = measures.tvoc_nox.nox_raw;
    ago = measures_to_ago(measures);
    has_measures = true;
    if (s_button_pressed) { promote = true; }
  }

  // --- One-shot GPS (skip if promoting) ---
  GpsData gps{};
  const bool gps_active =
      (ctx.settings.gps_mode == GpsMode::AlwaysOn) ||
      (ctx.settings.gps_mode == GpsMode::OnWhenTracking &&
       state.tracking_active);

  if (!promote && state.tracking_active && gps_active) {
    AirgradientUART gps_serial(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
    GpsDriver gps_driver(gps_serial);
    gps = gps_read_once(gps_driver, GPS_BAUD, 2000, s_button_pressed);
    if (s_button_pressed) { promote = true; }
  }

  // --- Storage + cache (skip if promoting) ---
  if (!promote) {
    init_storage(ctx);
    ctx.storage->cache_measurement(ago);

    if (state.tracking_active) {
      float battery_pct = -1.0f;
      ctx.bms->get_battery_percentage(&battery_pct);
      ctx.storage->start_route(state.tracking_session_id);
      RoutePoint point{};
      point.timestamp = time(nullptr);
      point.gps = gps;
      point.sensors = ago;
      point.battery_percentage = battery_pct;
      ctx.storage->append_route_point(point);
      ctx.storage->end_route();
    }
    ctx.storage->backup_cache();
  }

  // --- Power service ---
  if (!promote) {
    init_power(ctx);
    PowerSnapshot bms_snap = ctx.power_service->poll_bms();

    // --- Display ---
    init_display(ctx);
    DisplayValues values = build_fast_path_display(
        /* ... */ ago, gps, bms_snap, ctx.settings, state.tracking_active);
    ctx.display->init(values);

    // --- Sleep decision ---
    ctx.power_service->save_state(state);
    uint32_t awake_ms =
        static_cast<uint32_t>(RTOS::get_time_ms()) - boot_time_ms;
    auto decision = ctx.power_service->decide_sleep(
        ctx.settings, LockState::Locked, OperatingMode::Offline, awake_ms);

    if (decision.type == PowerService::SleepType::Deep) {
      ctx.display->stop();
      ctx.display->deep_sleep();
      gpio_isr_handler_remove(PIN_BUTTON_POWER);
      ctx.power_service->enter_sleep(decision.duration_ms);
      // Never returns — CPU reboots on wake.
    }

    // Sleep too short — promote to interactive, stay locked.
    promote = true;
  }

  // --- Remove ISR before InputService takes over ---
  gpio_isr_handler_remove(PIN_BUTTON_POWER);
  gpio_set_intr_type(PIN_BUTTON_POWER, GPIO_INTR_DISABLE);

  // --- Build handoff ---
  const bool button_caused_promote = s_button_pressed;

  BootHandoff handoff{};
  handoff.measurement_completed = has_measures;
  handoff.fast_path_measures = has_measures ? &ago : nullptr;

  if (button_caused_promote) {
    // User pressed button during fast path — unlock, suppress wake press.
    handoff.initial_lock_state = LockState::Unlocked;
    handoff.suppress_wake_press = true;
    // Display may or may not be initialized depending on where we
    // interrupted. If ctx.display is non-null, the display is showing
    // the locked fast-path frame — NOT valid for unlocked state.
    handoff.display_painted = false;
  } else {
    // Sleep too short — stay locked. Display shows correct locked frame.
    handoff.initial_lock_state = LockState::Locked;
    handoff.display_painted = (ctx.display != nullptr);
  }

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "fast-path promoting to interactive (button=%d)",
          static_cast<int>(button_caused_promote));
  run_interactive(WakeCause::Timer, ctx, handoff);
  // Never returns.
}
```

### run_button_wake_path() — uses shared helpers

The four-phase structure is preserved. The only change is replacing inline
init code with calls to shared helpers:

```
Phase 1: Early paint (~10 ms)
  init_spi(ctx)               // SPI bus only
  init_display(ctx)           // construct DisplayService
  load snapshot, build wake values
  ctx.display->init(wake_values, defer_refresh=true)

Phase 2: Parallel init (~300 ms)
  init_settings(ctx)          // NVS + settings
  init_buses(ctx)             // GPIO + I2C + delays
  init_bms_driver(ctx)        // BMS
  init_sensors(ctx)           // all sensor drivers
  GPS, touch, event queue, services, ui_manager — inline

Phase 3: Storage (~3 s blocking on SPI)
  init_storage(ctx)           // RTC cache + NAND (blocks until display done)
  BleService construction

Phase 4: Orchestrator
  Build BootHandoff{display_painted=true, suppress_wake_press=true,
                    initial_lock_state=Unlocked, display_snapshot=...}
  orchestrator->init(WakeCause::Button, handoff)
  orchestrator->run()
```

### run_interactive() — unified interactive entry

Handles both fresh boot (empty BootContext) and fast-path promotion
(partially filled BootContext).

```cpp
static void run_interactive(WakeCause cause, BootContext &ctx,
                            const BootHandoff &handoff) {
  // --- Complete any missing core init ---
  if (!ctx.config_store) {
    init_core(ctx);
  }
  if (!ctx.sensor_manager) {
    init_sensors(ctx);
  }

  // --- GPS driver (never done in fast path) ---
  auto *gps_serial =
      new AirgradientUART(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  auto *gps_driver = new GpsDriver(*gps_serial);

  // --- Touch sensor (never done in fast path) ---
  CAP1203::Config touch_cfg;
  touch_cfg.delta_sense = TOUCH_DELTA_SENSE;
  auto *touch = new CAP1203(ctx.i2c_bus, I2C_ADDR_CAP1203, touch_cfg);
  if (!touch->init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }

  // --- Storage (may already be done from fast path) ---
  if (!ctx.storage) {
    init_storage(ctx);
  }

  // --- Event queue ---
  RtosQueueHandle event_queue =
      RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // --- BLE ---
  auto *ble_service = new BleService(event_queue, *ctx.storage);

  // --- Service construction ---
  auto *sensor_producer = new SensorProducer(
      *ctx.sensor_manager, event_queue,
      {.task_stack_size = 4096, .task_priority = 5});

  auto *gps_service = new GpsService(
      *gps_driver, event_queue,
      {.baud_rate = GPS_BAUD,
       .posting_interval_ms = ctx.settings.gps_interval_seconds * 1000,
       .task_stack_size = 4096,
       .task_priority = 3});

  auto *input_service = new InputService(
      *touch, gpio::native::hal, event_queue,
      {.pin_cap_int = PIN_CAP_INT,
       .pin_button_power = PIN_BUTTON_POWER,
       .pin_button_boot = PIN_BUTTON_BOOT,
       .suppress_button_wake = handoff.suppress_wake_press});

  if (!ctx.display) {
    init_display(ctx);
  }

  if (!ctx.power_service) {
    init_power(ctx);
  }

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial_number.c_str());

  auto *ui_manager = new UIManager({
      .firmware_version = FIRMWARE_VERSION,
      .serial_number = serial_number.c_str(),
  });

  // --- Display init (if boot hasn't painted) ---
  if (!handoff.display_painted) {
    DisplayValues initial{};
    ctx.display->init(initial);
  }

  // --- Start producer tasks ---
  sensor_producer->start();
  gps_service->start();
  input_service->start();

  // --- Orchestrator ---
  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = *ctx.display,
      .storage_service = *ctx.storage,
      .power_service = *ctx.power_service,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
  };

  auto *orchestrator = new Orchestrator(
      event_queue, services, ctx.settings, *ctx.config_store,
      serial_number.c_str());
  orchestrator->init(cause, handoff);
  orchestrator->run(); // Never returns.
}
```

Note: `run_interactive()` conditionally calls `build_serial_number()`. For
fresh boot this was already called by `app_main()` and passed in. However,
the `run_interactive()` signature intentionally does NOT take a serial number
parameter — it calls `build_serial_number()` itself. This avoids adding
serial number management to `BootContext` or `run_fast_path()`. The
`build_serial_number()` call reads the MAC address, which is always
available. The cost is negligible.

## Edge Cases

| Scenario | Behavior |
|---|---|
| Fast path, sleep too short | Promotes to `run_interactive()` with BootContext. Device stays locked. Orchestrator has measures + `first_measurement_done = true`. Next sleep attempt happens on the next event loop iteration. |
| Fast path, button during warmup | Warmup aborted. No measurement. Promotes to `run_interactive()` unlocked. Display init'd fresh with empty values. Orchestrator requests measurement. |
| Fast path, button after measurement | Has measures but no display yet. Promotes unlocked. Display init'd fresh. Orchestrator seeds cached measures from fast path data. |
| Fast path, button after display | Has measures, display painted (locked). Promotes unlocked with `display_painted=false` (locked content is wrong for unlocked state). Orchestrator calls `unlock()` → `update_display()`. |
| Fast path, button during GPS read | GPS read aborted early. Has measures. Promotes unlocked. GPS data may be partial/empty — acceptable. |
| Button wake, Offline | Unchanged behavior: four-phase boot with early paint. |
| Button wake, non-Offline | Falls through to `run_interactive()` with empty context. Same as current `run_full_boot()`. |
| Power-on boot | Falls through to `run_interactive()` with empty context. Same as current `run_full_boot()`. |
| Timer wake, unlocked (not fast path) | `is_fast_path_wake()` returns false. Falls through to `run_interactive()`. |
| Double `gpio_install_isr_service()` | Idempotent in ESP-IDF — returns `ESP_ERR_INVALID_STATE` if already installed, which is not checked (safe). |
| ISR fires between removal and InputService | Flag is set but never read again. InputService reconfigures the GPIO independently. No conflict. |
| `gps_read_once` with default abort | Binds to a static `false` constant. Polling loop never exits early. Identical to current behavior. |
| Promotion with no storage init | `run_interactive()` checks `ctx.storage == nullptr` and calls `init_storage()`. Storage is always initialized before the Orchestrator. |
| Promotion with no power service | `run_interactive()` checks `ctx.power_service == nullptr` and calls `init_power()`. Power service is always initialized before the Orchestrator. |
| `warmup_step()` with no sensors | Returns immediately (same guard as `warmup()`). |

## Testability

### Existing tests — updated, not broken

All existing `sensor_manager.tests.cpp` warmup tests rename
`warmup_sensor()` → `warmup()`. Behavior is identical. No test logic
changes.

All existing `go_orchestrator.tests.cpp` `init()` calls update from
`init(cause, already_painted, snapshot)` to
`init(cause, BootHandoff{...})` with equivalent field values. Behavior
is identical. All existing assertions remain valid.

### New unit tests — sensor_manager.tests.cpp

| Test | Scenario | Assertion |
|---|---|---|
| `warmup_step — single cycle` | All sensors present | Exactly 1 `run_conditioning()` call + 1 `read()` per PM sensor. No `delay_ms()` call. |
| `warmup_step — no sensors` | All TVOC/PM null | Returns immediately. No sensor calls. |

### New unit tests — go_orchestrator.tests.cpp

| Test | Scenario | Assertion |
|---|---|---|
| `init(Timer, promoted, locked)` | Timer wake, `measurement_completed=true`, `initial_lock_state=Locked`, `fast_path_measures` set | RTC state restored. `_cached_measures` seeded. `_first_measurement_done = true`. `_lock_state = Locked`. No measurement requested. Route resumed if tracking active. |
| `init(Timer, promoted, unlocked)` | Timer wake, `measurement_completed=true`, `initial_lock_state=Unlocked`, `display_painted=false` | RTC state restored. `_lock_state = Unlocked`. `unlock()` called (triggers `update_display()`). Snackbar shown. `_first_measurement_done = true`. No measurement requested. |
| `init(Timer, promoted, unlocked, painted)` | Timer wake, `initial_lock_state=Unlocked`, `display_painted=true` | `_lock_state = Unlocked` set directly (no `update_display()`). Snackbar armed manually. |
| `init(Timer, promoted, no measures)` | Timer wake, `measurement_completed=false` | RTC state restored. `_first_measurement_done = false`. Measurement requested. |
| `init(PowerOn, default handoff)` | PowerOn, default `BootHandoff{}` | No RTC state restored. `_lock_state = Locked`. Measurement requested. Identical to current behavior. |
| `init(Button, display_painted + snapshot)` | Button wake with snapshot handoff | Identical to current `already_painted=true` behavior. Verifies backward compatibility. |

### Boot logic (not host-testable)

`run_fast_path()`, `run_button_wake_path()`, `run_interactive()`, and
`app_main()` contain hardware init calls and are not host-testable. The
testable logic they depend on is already covered:

- `PowerService::is_fast_path_wake()` — pure logic, testable
- `PowerService::decide_sleep()` — pure logic, testable
- `Orchestrator::init()` — tested via host tests
- `warmup_step()` / `warmup()` — tested via host tests

### Host-test build

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

### Firmware build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/go build
```

### Hardware verification (user-run, outside agent scope)

- **Fast path normal:** Timer wake → measure → display → sleep. Unchanged.
- **Fast path promotion (sleep too short):** Set a very short measurement
  interval. Timer wake → fast path completes → promotes to interactive →
  device enters event loop locked. No crash.
- **Fast path button interrupt (warmup):** Timer wake → during warmup
  (~10 s window), press power button → device promotes to interactive,
  unlocks, shows "Unlocked" snackbar.
- **Fast path button interrupt (GPS):** Timer wake with tracking active →
  during GPS read, press power button → device promotes to interactive,
  unlocks.
- **Button wake (Offline):** Unchanged four-phase boot. Early paint visible
  within ~10 ms of button press.
- **Power-on boot:** Unchanged behavior.
- **Button wake (Portable/Stationary):** Falls through to interactive boot.
  Unchanged behavior.

## Increments

The work is ordered for independent verification at each step:

1. **SensorManager warmup split** — `warmup_step()` + `warmup()`. No boot
   path changes. Tests updated. Firmware builds and tests pass.

2. **BootHandoff + Orchestrator signature** — Add `BootHandoff` to
   `go_types.h`. Change `Orchestrator::init()` signature and body. Update
   `main.cpp` call sites and all test `init()` calls. No boot path
   restructuring. Behavior identical to before. Tests pass.

3. **BootContext + shared helpers + run_interactive() + promotion** — The
   main structural refactor. Extract helpers. Replace `run_full_boot()` with
   `run_interactive()`. Wire fast-path promotion (sleep-too-short case only;
   ISR not yet added). Fix the latent crash. Firmware builds and boots
   correctly on all three paths.

4. **Fast-path interruptibility** — Add ISR setup, interruptible warmup
   loop, abort-aware GPS read, button-press promotion. Firmware builds.
   Hardware test: press button during fast path → device unlocks.

5. **New Orchestrator tests** — Add promotion test cases to
   `go_orchestrator.tests.cpp`. All tests pass.

Increments 1 and 2 can be done in parallel. Increment 3 depends on both.
Increment 4 depends on 3. Increment 5 depends on 2 and 3.

## Verification Checklist

- [ ] `idf.py -C products/go build` succeeds
- [ ] Host test suite builds and all existing tests pass
- [ ] `warmup_sensor()` removed; `warmup()` and `warmup_step()` present
- [ ] All `warmup_sensor()` test calls renamed to `warmup()`
- [ ] New `warmup_step()` tests pass
- [ ] `BootHandoff` struct defined in `go_types.h`
- [ ] `Orchestrator::init()` accepts `const BootHandoff &`
- [ ] All test `init()` calls updated to use `BootHandoff`
- [ ] New promotion test cases pass
- [ ] `BootContext` struct is file-local in `main.cpp`
- [ ] `run_full_boot()` removed; replaced by `run_interactive()`
- [ ] `run_fast_path()` never returns to `app_main()`
- [ ] `run_fast_path()` calls `run_interactive()` on promotion
- [ ] ISR installed before warmup, removed before `InputService` construction
- [ ] `gps_read_once()` accepts `const volatile bool &abort` with default
- [ ] No double hardware initialization on any boot path
- [ ] Button-wake four-phase structure preserved
- [ ] No magic numbers added
- [ ] `clang-format -style=file -i` applied to every modified source file
- [ ] Hardware test: fast path sleep-too-short → no crash, enters event loop
- [ ] Hardware test: button during fast path warmup → unlocks
- [ ] Hardware test: button wake in Offline → unchanged early paint behavior
- [ ] Hardware test: power-on boot → unchanged behavior

## What Is Not In This Spec

- Changes to `Orchestrator::run()`, `dispatch()`, `try_enter_sleep()`, or
  `prepare_for_sleep()`. The runtime event loop is untouched.
- Changes to any service interface other than `SensorManager` warmup rename
  and `gps_read_once()` abort parameter.
- A `BootController` class or formal boot state machine. Boot logic remains
  procedural functions in `main.cpp`.
- ULP wake source support. Noted as a possible future wake source but not
  in scope. If added later, it would resolve to the same boot intent
  categories (attended/unattended/fresh) and slot into `app_main()`'s
  existing wake-cause dispatch.
- Changes to the `InputService`, `DisplayService`, `PowerService`,
  `GpsService`, `StorageService`, or `BleService` public interfaces.
- Changes to `RtcAppState` or `RtcDisplaySnapshot` structs.
- Display refresh count optimization for the button-press-during-fast-path
  promotion case. The current approach may show a brief empty dashboard
  before the unlocked UI appears. This is acceptable for a first
  implementation and can be optimized later if needed.
- Test coverage for `run_fast_path()`, `run_button_wake_path()`,
  `run_interactive()`, or `app_main()` — these contain hardware init calls
  and are not host-testable. The decision logic they depend on is tested
  independently.
