# Button-Wake UX — Implementation Spec

Improve the deep-sleep button-wake experience in Offline mode.

## Goal

When the user presses the power button to wake from deep sleep in Offline
mode, show Home + Unlocked + notification in a single display refresh.
The current path takes ~4–5 seconds and flashes the display twice (once
with an empty frame, once with the actual content). The new path should
show useful content on the first and only refresh, and make the device
interactive as early as possible while the display is still refreshing.

## Current Behavior

| Wake source | Condition | Path | First meaningful paint |
|---|---|---|---|
| Timer | Locked | `run_fast_path()` | ~3 s (full refresh) |
| Button | Any | `run_full_boot()` | ~4–5 s (empty frame + unlock refresh) |
| Power-on | — | `run_full_boot()` | ~4–5 s |

Button wake goes through `run_full_boot()` which initializes all hardware
and services, does a full display refresh with empty values, then the
orchestrator calls `unlock()` which triggers a second refresh with the
actual Home + Unlocked content.

The user sees two display flashes and waits through full initialization
before the device looks interactive.

## Target Behavior

| Wake source | Condition | Path | First meaningful paint |
|---|---|---|---|
| Timer | Locked | `run_fast_path()` | ~3 s (unchanged) |
| Button | Offline mode | `run_button_wake_path()` | ~3 s (single useful refresh) |
| Button | Other modes | `run_full_boot()` | ~4–5 s (unchanged) |
| Power-on | — | `run_full_boot()` | ~4–5 s (unchanged) |

The button-wake path renders the frame and hands off the SPI refresh to
the display worker task. While the e-paper refreshes (~3 s), the main
task runs full hardware and service initialization in parallel. Touch
input and sensors are ready within ~300 ms. NAND storage initializes
after the display releases the SPI bus. The orchestrator starts as soon
as everything is ready.

## RTC Memory Budget

| Variable | Size | Location | Status |
|---|---|---|---|
| `RtcAppState` + valid flag | ~13 B | `go_power.cpp` | Existing |
| `PayloadCacheStorageData` | ~1.5 KB | `rtc_payload_cache_storage.cpp` | Existing |
| RTC display snapshot + valid flag | ~50 B | `go_display.cpp` | New |
| **Total** | **~1.6 KB** | | |

ESP32-C5 RTC slow memory: 8 KB. Headroom: ~6.4 KB.

## Files

| File | Change |
|---|---|
| `go_display.h` | `init()` adds `defer_refresh` parameter, `deep_sleep()` method |
| `go_display.cpp` | RTC display snapshot state, deferred initial refresh via worker, `deep_sleep()` |
| `main.cpp` | `run_button_wake_path()`, updated boot routing in `app_main()` |
| `go_orchestrator.h` | `init()` adds `bool already_painted` parameter |
| `go_orchestrator.cpp` | `init()` skips `unlock()` + `update_display()` when already painted; `prepare_for_sleep()` saves snapshot + calls `deep_sleep()` |
| `go_input.h` | Method or parameter for wake-press suppression |
| `go_input.cpp` | Suppress first power button event after button wake |

---

## RTC Display Snapshot

A small struct containing only scalar, fixed-size data needed to render
the Home screen without reading NVS or sensors. Required fields:

- Sensor values: CO2, PM2.5, temperature, humidity, TVOC, NOx, pressure,
  altitude
- Clock: hour, minute
- Battery: percentage, charging state
- Status flags: GPS enabled, GPS fix, tracking active, BLE enabled
- Rendering settings: use_fahrenheit, pm_use_usaqi

Exact struct definition left to implementation. Estimated size: ~50 bytes.

Saved in `prepare_for_sleep()` after the final display update, alongside
the existing `RtcAppState`. A separate validity flag gates usage — on
first power-on (zero-initialized RTC), the snapshot is invalid and the
button-wake path renders with invalid sentinels (dashes).

## Boot Path Routing

```
app_main():
    RTOS::delay_ms(100)
    cause = PowerService::get_wake_cause()

    if cause == Timer:
        state = load_rtc_app_state()
        if state valid and state.lock_state == Locked:
            run_fast_path(state)            // never returns

    if cause == Button:
        state = load_rtc_app_state()
        if state valid and state.mode == Offline:
            run_button_wake_path(state)     // never returns

    run_full_boot(cause, serial)            // never returns
```

## DisplayService API Changes

### `init()` — Add `defer_refresh` Parameter

```cpp
void init(const DisplayValues &values, bool defer_refresh = false);
```

When `defer_refresh` is `false` (default): unchanged behavior —
initializes display hardware, renders, performs a synchronous full
refresh, then starts the async worker task. Backward compatible for
`run_full_boot()` and `run_fast_path()`.

When `defer_refresh` is `true`: initializes display hardware, renders
the frame into the software buffer, starts the worker task, and posts
the initial full refresh as the worker's first job. Returns immediately
(~10 ms) without waiting for the refresh to complete. The e-paper
refresh runs in the background on the worker task.

The worker acquires the SPI bus for the duration of the refresh (~3 s).
Any other SPI device transactions (NAND) naturally block until the
worker releases the bus. No explicit synchronization is needed — the
SPI bus serialization handles it.

### New Method: `deep_sleep()`

Puts the SSD1680 into deep sleep mode 1. Preserves display RAM, reduces
quiescent current from ~100 µA to <1 µA. On next wake,
`driver_hw_init_full()` exits deep sleep via hardware reset.

Called from `prepare_for_sleep()` after stopping the worker task. Minor
power improvement, not directly related to button-wake UX but essentially
free to add.

## `run_button_wake_path()` Flow

Standalone function in `main.cpp`. Does not call `run_full_boot()`.
Four phases: paint, parallel init, storage, orchestrator.

### Phase 1: Early Paint (~10 ms)

Render the wake frame and hand off the SPI refresh to the display worker.

```
run_button_wake_path(state):
    // --- Phase 1: Early paint ---
    init_spi_buses()                        // ~1 ms
    display = new DisplayService(config)

    snapshot = load_rtc_display_snapshot()
    values = build_wake_values(snapshot)
    //   screen      = Home
    //   locked      = false
    //   snackbar    = "Unlocked"
    //   sensor data = from snapshot (or invalid sentinels if invalid)
    //   status      = from snapshot
    //   display_off = false (always show on interactive wake)

    display->init(values, /* defer_refresh= */ true)
    // Returns in ~10 ms. Worker handles SPI refresh in background (~3 s).
```

`build_wake_values()` constructs a `DisplayValues` from the RTC snapshot:

- Sensor values from snapshot, or invalid sentinels when snapshot is
  invalid (display shows dashes)
- `screen = Home`, `locked = false`, `display_off = false`
- `snackbar_text = "Unlocked"` (static string literal)
- Status flags and rendering settings from snapshot

The display only needs the SPI bus to be initialized. No NVS, no GPIO
(PM sensor power), no I2C. The RTC snapshot provides all data needed for
rendering.

### Phase 2: Parallel Init (~300 ms)

Run all non-SPI initialization while the display refreshes in the
background. Nothing in this phase uses SPI.

```
    // --- Phase 2: Parallel init (runs while display refreshes) ---
    init_nvs()
    settings = load_settings()
    init_gpio()
    init_i2c_bus()                          // + settling delay
    // SPI bus already initialized in Phase 1
    BMS init                                // I2C
    Sensor drivers + SensorManager          // I2C / UART
    GPS serial + driver                     // UART
    Touch sensor                            // I2C
    PayloadCache + restore_cache()          // RTC memory, no SPI
    Event queue
    BLE service

    Construct services (reuse display instance from Phase 1)
    Start producer tasks (sensor, GPS, input)
    // Touch input and sensors operational from here (~310 ms)
```

The `DisplayService` from Phase 1 is passed into the `Services` struct.
All peripherals in this phase use I2C or UART — no SPI contention with
the display worker.

The payload cache and chart data use RTC memory (`RtcPayloadCacheStorage`),
not NAND flash. `restore_cache()` is a `memcpy` from RTC statics — no
SPI involved.

### Phase 3: Storage Init (after display refresh)

NAND flash shares the SPI bus with the display. Its initialization
naturally blocks until the display worker releases the bus.

```
    // --- Phase 3: Storage init (blocks until SPI free) ---
    SpiNandStorage construction + config
    storage->init()                         // NAND mount — blocks on SPI
    // Completes as soon as display refresh finishes (~3 s from Phase 1)
```

No explicit wait or synchronization is needed. `storage->init()` calls
`spi_device_transmit()` internally, which blocks while the display worker
holds the SPI bus via `spi_device_acquire_bus()`. Once the display
refresh finishes and the worker releases the bus, NAND initialization
proceeds immediately.

### Phase 4: Orchestrator Start

```
    // --- Phase 4: Orchestrator ---
    orchestrator = new Orchestrator(services, settings, ...)
    orchestrator->init(WakeCause::Button, /* already_painted= */ true)
    orchestrator->run()                     // never returns
```

### Timeline

```
0 ms      Phase 1: SPI init, render, worker starts refresh
10 ms     Phase 2: NVS, I2C, BMS, sensors, GPS, touch, BLE...
310 ms    Phase 2 done: touch input ready, sensors measuring
          Phase 3: storage->init() called, blocks on SPI bus
~3,000 ms Display refresh done, SPI bus released
~3,010 ms NAND mount completes
~3,020 ms Orchestrator init + run
~3,000 ms User sees screen (Home + Unlocked + notification)
```

## Orchestrator Changes

### `init()` — Add `already_painted` Parameter

```cpp
void init(WakeCause cause, bool already_painted = false);
```

When `cause == Button` and `already_painted == true`:

- Restore state from RTC (behavior, GPS, tracking) — same as today
- Set `_lock_state = Unlocked` directly (do NOT call `unlock()`)
- Set `_last_input_ms` to current time (start auto-lock timer)
- Call `ui_manager.show_snackbar("Unlocked")` to arm the snackbar timer
- Request fresh measurement in background
- Do NOT call `update_display()` — the screen already shows the correct
  content
- Skip the final `update_display()` at the end of `init()` as well

When `already_painted == false`: unchanged behavior (calls `unlock()` and
`update_display()` as today).

The first live display update happens when sensor data arrives or a timer
fires in the orchestrator event loop.

#### Snackbar Timing

The snackbar is rendered in the early paint but the UIManager timer is
not armed until `show_snackbar()` is called in the orchestrator init.
The 3-second countdown starts from the first `clear_expired_snackbar()`
call in the orchestrator event loop (~3 s after boot). The snackbar is
visible from when the user sees the screen (~3 s) until the timer
expires (~6 s). This is acceptable — the notification is informational.

### `prepare_for_sleep()` — Save Snapshot + Display Deep Sleep

After the final `update(values, true)`:

1. Save RTC display snapshot from the current display values
2. After `display_service.stop()` — call `display_service.deep_sleep()`

## InputService: Wake-Press Suppression

The power button press that wakes the device from deep sleep must not
generate a spurious input event after `InputService` starts.

The orchestrator already handles the wake unlock explicitly
(`_lock_state = Unlocked`). A spurious power button ShortPress from the
wake press could trigger a re-lock.

`InputService` must suppress the first power button press event when
started after a button wake. Implementation: accept a flag or parameter
indicating the wake source, and discard the first power button event
if it occurs. Touch pad events are unaffected.

## Edge Cases

| Scenario | Behavior |
|---|---|
| RTC snapshot valid | Full refresh with cached sensor values (~3 s) |
| RTC snapshot invalid (first power-on) | Full refresh with invalid sentinels — dashes (~3 s) |
| User doesn't interact after wake | Normal auto-lock timeout applies |
| User presses touch pad during refresh | Touch registered at ~310 ms, display update queued behind initial refresh |
| Offline mode changed via BLE before sleep | RTC snapshot has correct mode; routing uses RTC state |
| `display_off` was true in snapshot | Ignored — wake always shows Home (interactive context) |
| Fresh measurement arrives | Orchestrator calls `update_display()` with live values, queued after initial refresh |
| Tracking was active before sleep | Route resume in orchestrator `init()` works — NAND initialized in Phase 3 |
| SPI bus contention | Display worker holds bus during refresh; NAND init in Phase 3 blocks naturally until bus is free |

## Expected Impact

| Metric | Before | After |
|---|---|---|
| Time to first meaningful paint | ~4–5 s | ~3 s |
| Number of display refreshes on wake | 2 (empty + unlock) | 1 (useful content) |
| Display flash quality | Empty frame flash + content flash | Single content flash |
| Touch input ready | ~4–5 s | ~310 ms |
| Sensors measuring | ~4–5 s | ~310 ms |
| Full initialization complete | ~4–5 s | ~3 s |
| SSD1680 during ESP deep sleep | ~100 µA | <1 µA (with `deep_sleep()`) |

---

## Design Decisions

### Standalone `run_button_wake_path()`

The button-wake path is a standalone function that inlines the full
initialization sequence. It does not call or modify `run_full_boot()`.
This avoids coupling to `run_full_boot()` internals and makes the
early-paint flow explicit.

### Deferred Refresh via Worker Task

The display worker task already exists for handling async display updates.
By posting the initial full refresh as the worker's first job, `init()`
returns immediately and the main task can continue with hardware
initialization. The SPI bus is held by the worker during the refresh,
which naturally serializes NAND storage initialization without requiring
explicit synchronization.

### NAND After Display, Not Skipped

NAND storage is initialized after the non-SPI init completes. It blocks
on the SPI bus until the display refresh finishes. This is simpler and
more correct than skipping NAND — tracking, route persistence, and BLE
history export all work without special handling. The ~2.7 s wait on
SPI is not wasted: touch input, sensors, and GPS are already running
in parallel.

### `already_painted` as Parameter, Not RTC Field

The orchestrator only needs to know about the early paint once during
`init()`. A function parameter is simpler and avoids adding transient
state to the persisted `RtcAppState` struct.

### Full Refresh, Not Partial

The button-wake path always does a full e-paper refresh. Partial refresh
would require persisting the 4,000-byte framebuffer in RTC memory as a
basemap — significant complexity for a ~2 second improvement. Full
refresh is simpler and eliminates the double-flash problem. Partial
refresh can be added later as an optimization if needed.

### Save RTC Snapshot Only Before Sleep

The snapshot only needs to reflect the last displayed state before sleep.
One save point in `prepare_for_sleep()` is sufficient. No snapshot saves
during normal operation.

## Future Optimization

If ~3 seconds is still too slow, the next step is to add RTC framebuffer
persistence (~4 KB) and partial refresh support. This would bring the
button-wake first paint down to ~0.5–1 second. See
`display_sleep_optimization.md` for the RTC frame infrastructure design
that would enable this.

## Non-Goals

- Timer-wake display optimization (separate concern)
- Light-sleep UX
- Full menu or navigation state restoration across deep sleep
- Generic boot-time optimization for all modes
- Display animation or visual effects
- Precise snackbar timing on button wake
