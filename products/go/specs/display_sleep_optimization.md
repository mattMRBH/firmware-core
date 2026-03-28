# Display Sleep Optimization — Implementation Spec

Eliminate unnecessary e-paper refreshes on timer wake from deep sleep.
Currently, every fast-path wake performs a full SSD1680 hardware init and
full-screen refresh (~2–3 s visible flash), even when the displayed content
has not changed. This spec introduces three optimizations:

1. **Skip** the refresh entirely when the rendered frame is identical to what
   is already on screen.
2. **Partial refresh** when only the body region changed (no full-screen
   flash, ~0.5–1 s).
3. **Minor power/latency fixes**: SSD1680 deep sleep before ESP deep sleep,
   eliminate unnecessary worker task creation in fast path.

## Background

The fast-path boot (`run_fast_path()` in `main.cpp`) is the primary timer
wake path. It measures sensors, renders a frame, refreshes the e-paper,
and re-enters deep sleep. The display refresh dominates the awake time:

| Operation | Duration |
|---|---|
| NVS + settings | ~5 ms |
| GPIO + settling delay | ~1100 ms |
| I2C + SPI bus init | ~5 ms |
| Sensor measurement (1 iter) | ~50 ms |
| Display: driver init + SSD1680 HW init | ~30 ms |
| Display: SPI transfer (8000 B @ 4 MHz) | ~20 ms |
| Display: full update waveform | **~2000–3000 ms** |
| Sleep decision + enter sleep | ~1 ms |

The full update waveform causes a visible black → white → black flash and
is the most power-expensive operation in the cycle. E-paper is persistent —
it retains its image without power. If the content hasn't changed, the
refresh is pure waste.

### Current Fast-Path Display Flow

```
display->init(values)
  ├── driver_init()            GPIO, SPI device, DMA buffer
  ├── u8g2 setup               software renderer
  ├── _render_frame(values)    draw into _render_buf
  ├── driver_bus_acquire()
  ├── driver_hw_init_full()    HW reset + register config
  ├── driver_set_basemap()     write both RAM planes + full update
  ├── driver_bus_release()
  └── start worker task        4 KB stack, never used in fast path
```

Problems:
1. Always does a full refresh, even when nothing changed
2. No mechanism to detect content changes across deep sleep
3. Starts an async worker task that is immediately abandoned (CPU reboots)
4. SSD1680 is left in idle mode during ESP deep sleep (draws quiescent
   current instead of ~<1 µA in deep sleep mode 1)

## Files

| File | Change |
|---|---|
| `products/go/main/go_display.h` | Add `RefreshResult` enum, `init_fast_path()` method, TEST_HOST stub |
| `products/go/main/go_display.cpp` | RTC frame state, `init_fast_path()`, `driver_partial_from_basemap()`, save RTC frame in existing refresh paths |
| `products/go/main/main.cpp` | Replace `display->init(values)` with `init_fast_path(values)` in `run_fast_path()` |
| `products/go/main/go_orchestrator.cpp` | Add `display_service.deep_sleep()` in `prepare_for_sleep()` |

## Dependencies

No new dependencies. All changes use existing ESP-IDF APIs and the
existing SSD1680 driver infrastructure in `go_display.cpp`.

## Design Decisions

### Framebuffer Comparison, Not DisplayValues Comparison

The skip/partial decision is based on `memcmp` of rendered framebuffers,
not on comparing `DisplayValues` fields. Reasons:

1. `DisplayValues` contains pointers (`chart_samples`, `about_title`) that
   cannot be meaningfully compared across reboots.
2. Framebuffer comparison is decoupled from rendering logic — any rendering
   change is automatically detected.
3. The same 4000-byte frame stored for skip detection doubles as the
   basemap source for partial refresh.

### Full Frame in RTC, Not Hash Only

A 4-byte hash (CRC32) would suffice for skip detection but cannot serve as
a basemap for partial refresh. Storing the full 4000-byte frame enables
both features with a single RTC allocation. The RTC memory budget allows
it (see below).

### Don't Trust SSD1680 RAM Across Deep Sleep

The SSD1680 controller likely retains its RAM across ESP32 deep sleep
(VDD stays powered from battery). However, relying on this is fragile:
a brief power glitch, brown-out, or future use of SSD1680 deep sleep
mode 2 (which does NOT preserve RAM) would silently corrupt the basemap
and produce display artifacts. Storing the authoritative basemap in
ESP32 RTC memory is more robust. The SSD1680 RAM is treated as untrusted
after every deep sleep wake.

### Partial Count Persisted Across Sleep Cycles

E-paper partial refreshes accumulate ghosting. The existing
`max_partial_ops` (default 20) forces a periodic full refresh. This
counter must persist across deep sleep cycles; otherwise the device could
do unlimited partials (one per wake) without ever forcing a full refresh.
The counter is stored in RTC memory alongside the frame.

### RTC State Owned by DisplayService

The RTC frame state (`s_rtc_frame`, `s_rtc_partial_count`,
`s_rtc_frame_valid`) is declared as file-local `RTC_DATA_ATTR` statics
in `go_display.cpp`, following the same pattern as `s_rtc_state` in
`go_power.cpp`. This keeps RTC frame management encapsulated within the
display module. Both the fast-path method and the normal worker update
the same state, ensuring consistency regardless of boot path.

### No New DisplayService Config Fields

The existing `Config::max_partial_ops` (default 20) controls the partial
refresh limit in both the normal async path and the new fast-path. No
additional configuration is needed.

## RTC Memory Budget

| Variable | Size | Location |
|---|---|---|
| `RtcAppState` + valid flag | ~13 B | `go_power.cpp` (existing) |
| `PayloadCacheStorageData` | ~1.2 KB | `rtc_payload_cache_storage.cpp` (existing) |
| **`s_rtc_frame[4000]`** | **4000 B** | `go_display.cpp` (new) |
| **`s_rtc_partial_count`** | **1 B** | `go_display.cpp` (new) |
| **`s_rtc_frame_valid`** | **1 B** | `go_display.cpp` (new) |
| **Total** | **~5.2 KB** | |

ESP32-C5 RTC slow memory is at least 8 KB. The 5.2 KB total leaves
~2.8 KB headroom for future use.

On first power-on, `RTC_DATA_ATTR` variables are zero-initialized by the
bootloader. `s_rtc_frame_valid` starts as `false`, so the first fast-path
wake falls through to a full refresh. No explicit invalidation needed.

## DisplayService API Changes

### New Enum

```cpp
enum class RefreshResult : uint8_t {
    Skipped,  ///< Frame unchanged — no SPI activity
    Partial,  ///< Body-only partial refresh
    Full,     ///< Full-screen refresh
};
```

### New Method

```cpp
/// Fast-path synchronous update with skip/partial/full decision.
/// Self-contained: sets up u8g2, renders frame, initializes SPI driver
/// only if refresh is needed. Does NOT start the async worker task.
/// Returns the type of refresh performed.
RefreshResult init_fast_path(const DisplayValues &values);
```

### Existing Methods — Internal Changes

| Method | Change |
|---|---|
| `init()` | After initial full refresh: save `_render_buf` to `s_rtc_frame`, set `s_rtc_frame_valid = true`, reset `s_rtc_partial_count = 0` |
| `_worker_loop()` | After each successful refresh: save `_spi_buf` to `s_rtc_frame`, update `s_rtc_partial_count` (reset on full, increment on partial) |
| `update_sync()` | After refresh: same as `init()` |

These changes ensure the RTC frame is always in sync with the last
displayed content, regardless of whether the device entered via fast-path
or full boot. When `prepare_for_sleep()` calls `update(values, true)` and
then `stop()`, the worker has already saved the final frame to RTC.

### TEST_HOST Stub

```cpp
#ifdef TEST_HOST
class DisplayService {
public:
    // ... existing stubs ...
    enum class RefreshResult : uint8_t { Skipped, Partial, Full };
    RefreshResult init_fast_path(const DisplayValues &) { return RefreshResult::Full; }
};
#endif
```

## New Driver Function

```cpp
/// Load basemap from external buffer into SSD1680 RAM, then perform a
/// full-frame partial refresh with new content.
/// Caller must have called driver_hw_init_full() first.
///
/// Sequence:
///   1. Reset RAM counter to (0, HEIGHT_PX - 1)
///   2. Write basemap to RAM plane 0x26 (4000 bytes)
///   3. Reset RAM counter to (0, HEIGHT_PX - 1)
///   4. Write new_frame to RAM plane 0x24 (4000 bytes)
///   5. Set border waveform for partial mode (0x3C, 0x80)
///   6. Trigger partial update (0x22 + 0xFF, 0x20)
///   7. Wait for BUSY low
esp_err_t driver_partial_from_basemap(const uint8_t *basemap,
                                      const uint8_t *new_frame);
```

This writes the full frame (4000 bytes) to each plane rather than just
the body region. The SSD1680 compares both planes pixel-by-pixel; pixels
that are identical in both planes are not driven. Since we only call this
when the header is unchanged, header pixels are identical and not
refreshed.

Writing full frames is simpler than managing sub-region RAM windows and
adds only 640 bytes of SPI overhead (320 bytes header × 2 planes). At
4 MHz SPI, this costs ~1.3 ms — negligible compared to the partial
waveform time (~500 ms).

## init_fast_path() Logic

```
init_fast_path(values):

    // 1. Set up u8g2 software renderer (no SPI, no GPIO)
    u8g2_SetupDisplay(...)
    u8g2_SetupBuffer(...)
    u8g2_SetFontMode(...)

    // 2. Render new frame into _render_buf
    _render_frame(values)

    // 3. Skip detection — identical frame
    if s_rtc_frame_valid
       and memcmp(_render_buf, s_rtc_frame, FRAME_BYTES) == 0:
        _prev_values = values
        return RefreshResult::Skipped

    // 4. Initialize display hardware (GPIO, SPI device, DMA)
    err = driver_init(_config)
    if err != ESP_OK:
        return RefreshResult::Full    // error, best-effort

    // 5. Try partial refresh if conditions met
    if s_rtc_frame_valid
       and s_rtc_partial_count < _config.max_partial_ops:

        // Compare header region only (rows 0..19 = 320 bytes)
        header_bytes = BODY_Y * BUF_ROW_BYTES    // 20 * 16 = 320
        if memcmp(_render_buf, s_rtc_frame, header_bytes) == 0:

            err = driver_bus_acquire()
            if err == ESP_OK:
                err = driver_hw_init_full()
            if err == ESP_OK:
                err = driver_partial_from_basemap(s_rtc_frame, _render_buf)
            driver_bus_release()

            if err == ESP_OK:
                memcpy(s_rtc_frame, _render_buf, FRAME_BYTES)
                s_rtc_partial_count++
                _prev_values = values
                return RefreshResult::Partial

            // Fall through to full refresh on SPI error

    // 6. Full refresh (first boot, header changed, max partials, or error)
    err = driver_bus_acquire()
    if err == ESP_OK:
        err = driver_hw_init_full()
    if err == ESP_OK:
        err = driver_set_basemap(_render_buf)
    driver_bus_release()

    memcpy(s_rtc_frame, _render_buf, FRAME_BYTES)
    s_rtc_frame_valid = true
    s_rtc_partial_count = 0
    _prev_values = values
    return RefreshResult::Full
```

### Decision Matrix

| RTC valid | Header changed | Partial count | Frame changed | Action |
|---|---|---|---|---|
| No | — | — | — | Full refresh |
| Yes | — | — | No | **Skip** |
| Yes | No | < max | Yes | **Partial refresh** |
| Yes | Yes | — | Yes | Full refresh |
| Yes | No | >= max | Yes | Full refresh |

## main.cpp Changes

### run_fast_path()

```
// Was:
auto *display = new DisplayService(display_config)
DisplayValues values = build_fast_path_display(measures, gps, bms_snap, settings)
display->init(values)

// Now:
auto *display = new DisplayService(display_config)
DisplayValues values = build_fast_path_display(measures, gps, bms_snap, settings)
display->init_fast_path(values)
```

The return value (`RefreshResult`) can be logged for diagnostics but does
not affect control flow — the fast path always proceeds to sleep
regardless of refresh outcome.

## go_orchestrator.cpp Changes

### prepare_for_sleep()

Add `deep_sleep()` call after stopping the worker task:

```
prepare_for_sleep():
    // ... render final frame, wait for worker ...
    _svc.display_service.update(values, true)

    // ... stop services ...
    _svc.display_service.stop()
    _svc.display_service.deep_sleep()    // NEW: SSD1680 mode 1

    // ... backup cache, save state, reset ext WDT ...
```

SSD1680 deep sleep mode 1 preserves RAM and reduces quiescent current
from ~100 µA to <1 µA. On next wake, `driver_hw_init_full()` begins with
a hardware reset which exits deep sleep mode. No additional wake sequence
needed.

The worker's final refresh (triggered by `update(values, true)`) has
already saved the frame to `s_rtc_frame`. Calling `deep_sleep()` after
`stop()` does not affect the RTC state.

## Edge Cases

| Scenario | Behavior |
|---|---|
| First power-on | `s_rtc_frame_valid == false` → full refresh, same as today |
| Button wake → full boot | `init()` does full refresh and saves to RTC. Next timer wake has valid baseline. |
| `display_off == true` | Frame is always the same (logo). Skip after first render — zero refresh on all subsequent wakes. |
| Partial count at max | Forces full refresh, resets counter. Prevents ghosting accumulation. |
| SPI bus error during partial | Falls through to full refresh attempt. Best-effort recovery. |
| Battery % changes (header) | Header region differs → full refresh (cannot partial). |
| Only sensor value changes | Body region differs, header same → partial refresh. |
| GPS minute rolls over | Clock is in header → header differs → full refresh. |
| All values identical | Full frame `memcmp` matches → skip. |
| Firmware update | RTC memory may contain stale frame from old firmware. `s_rtc_frame_valid` persists, so first wake may do a partial with a stale basemap. Worst case: one display artifact, corrected by the next full refresh (max partial count). Acceptable for a rare event. |
| Light sleep wake | Light sleep preserves RAM — no RTC frame needed. Display worker restarts normally. No change to light sleep path. |

## Expected Impact

| Scenario | Before | After |
|---|---|---|
| Values unchanged | Full init + full refresh (~3 s) | Skip (~0 ms display time) |
| Only body values changed | Full init + full refresh (~3 s) | Partial refresh (~0.5–1 s, no flash) |
| Header changed (clock, battery) | Full init + full refresh (~3 s) | Full refresh (~3 s, same) |
| `display_off` mode | Full init + full refresh (~3 s) | Skip after first boot (~0 ms) |
| SSD1680 quiescent during sleep | ~100 µA | <1 µA (deep sleep mode 1) |
| Fast-path heap usage | +4 KB worker stack (wasted) | No worker task created |

The biggest wins are for short measurement intervals where sensor values
rarely change between cycles, and for `display_off` mode where the display
never changes after the initial boot.

## Testability

### Unit-Testable Logic

The skip/partial/full decision can be extracted into a pure function for
host testing:

```cpp
/// Pure decision function — no hardware dependencies.
RefreshResult decide_refresh(const uint8_t *old_frame,
                             bool old_valid,
                             const uint8_t *new_frame,
                             size_t frame_bytes,
                             size_t header_bytes,
                             uint8_t partial_count,
                             uint8_t max_partial_ops);
```

Under `TEST_HOST`, `RTC_DATA_ATTR` is defined away. The RTC frame
variables become regular statics, so `init_fast_path()` can be tested
in sequence:

1. First call: `s_rtc_frame_valid == false` → returns `Full`
2. Same values again: frame matches → returns `Skipped`
3. Different body value: body differs → returns `Partial`
4. Different header value: header differs → returns `Full`
5. Repeat partials to max → returns `Full` (counter reset)

### Test Scenarios

| Scenario | Verify |
|---|---|
| First call (no RTC state) | Returns `Full`, sets `s_rtc_frame_valid` |
| Identical frame | Returns `Skipped`, no driver_init called |
| Body-only change, count < max | Returns `Partial`, count incremented |
| Body-only change, count >= max | Returns `Full`, count reset to 0 |
| Header change | Returns `Full`, count reset to 0 |
| Driver init failure | Returns `Full` (error path) |
| `init()` saves RTC frame | After `init()`, `s_rtc_frame_valid == true` |
| Worker saves RTC frame | After worker refresh, `s_rtc_frame` updated |
| `prepare_for_sleep` + deep_sleep | `deep_sleep()` called after `stop()` |

## What Is Not In This Spec

- No change to the full-boot display path (init + worker loop) beyond
  saving the RTC frame after each refresh
- No change to the partial refresh logic in the normal async worker
  (body-region-only partial with existing header-change detection)
- No change to `DisplayValues`, `Screen`, or `UIManager`
- No change to `GoSettings` or sleep duration calculations
- No SPS30 power-up delay optimization (can be done separately)
- No display_refresh_interval interaction changes — the interval still
  controls whether the display is refreshed at all; this spec only
  optimizes *how* it refreshes when it does
