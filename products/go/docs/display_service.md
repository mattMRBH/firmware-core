# Display Service

Product-specific display service for AirGradient Go. Manages e-paper rendering
via u8g2, three-tier refresh decisions (full/fast/partial), and an async worker
task for SPI hardware refresh. The orchestrator calls the Display Service API
which returns immediately; the slow EPD refresh runs in a dedicated task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_display.h` | `DisplayService` class, `DisplayValues` struct, `RtcDisplaySnapshot` struct, `Screen`/`Metric` enums, free functions |
| `products/go/main/go_display.cpp` | Rendering pipeline, refresh logic, worker task, display driver, RTC snapshot storage |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `u8g2` | `components/u8g2/` | Software rendering (fonts, primitives, framebuffer) |
| `go_types.h` | product | `OperatingMode`, `LockState` enums |
| `measures_types.h` | `airgradient-common` | `MeasuresInvalid` sentinel values |
| ESP-IDF SPI driver | ESP-IDF | `spi_device_*` API for EPD communication |
| ESP-IDF GPIO driver | ESP-IDF | CS, DC, RST, BUSY pin control |
| RTOS task/notification | `airgradient-common` | Worker task, frame-ready signaling |

## Display Hardware

**Panel:** Good Display GDEY0213B74, 128x250 px, 1-bit monochrome (SSD1680).
**Interface:** SPI half-duplex, mode 0, 4 MHz, CS managed manually.
**SPI bus:** Shared with NAND flash on SPI2_HOST; uses
`spi_device_acquire_bus(portMAX_DELAY)` for exclusive access during display
operations (ESP-IDF only supports infinite wait for bus acquisition).

All pin assignments come from `board_config.h` via `Config` struct members.

## Public API

### DisplayService::Config

| Field | Type | Default | Purpose |
|---|---|---|---|
| `spi_host` | `spi_host_device_t` | (required) | SPI bus host |
| `pin_cs` | `int` | (required) | Chip select GPIO |
| `pin_dc` | `int` | (required) | Data/command GPIO |
| `pin_rst` | `int` | (required) | Hardware reset GPIO |
| `pin_busy` | `int` | (required) | Busy status GPIO (input) |
| `clock_hz` | `int` | 4000000 | SPI clock frequency |
| `task_stack_size` | `uint16_t` | 4096 | Worker task stack size |
| `task_priority` | `uint8_t` | 4 | Worker task RTOS priority |
| `max_partial_ops` | `uint8_t` | 20 | Differential op limit before forced full GC |

### Methods

| Method | Blocking | Description |
|---|---|---|
| `init(initial, defer_refresh=false)` | Depends | Init driver + u8g2, render initial frame. See below. |
| `update(values, wait)` | No* | Render frame, signal worker. `wait=true` blocks until worker ready |
| `update_sync(values)` | Yes | Render + SPI inline; for fast-path boot without worker |
| `clear()` | Yes | Clear display to white via full refresh |
| `deep_sleep()` | Yes | Put SSD1680 into deep sleep mode 1 (~100 µA → <1 µA) |
| `stop()` | Yes | Stop worker task; call before ESP deep sleep |

#### `init()` — `defer_refresh` parameter

```cpp
bool init(const DisplayValues &initial, bool defer_refresh = false);
```

| `defer_refresh` | Behavior |
|---|---|
| `false` (default) | Synchronous: renders frame, performs full SPI refresh (~3 s), then starts worker. Used by `run_interactive()` and `run_fast_path()`. |
| `true` | Deferred: renders frame into buffer, copies to SPI buffer, marks full refresh pending, starts worker and immediately signals it to run the initial refresh in the background. Returns in ~10 ms. |

When `defer_refresh=true`, `init()` returns before the SPI refresh begins.
The worker task acquires the SPI bus and holds it for the duration of the
refresh (~3 s). Any other SPI device that calls `spi_device_transmit()` during
this window (e.g. NAND flash) blocks until the worker releases the bus —
natural serialization without an explicit semaphore.

`_worker_busy` is set to `true` before the worker starts so that a concurrent
`update()` call will not corrupt the in-progress refresh.

## RTC Display Snapshot

`RtcDisplaySnapshot` is a flat struct of scalar fields (no pointers) stored in
RTC slow memory so it survives deep sleep. It is saved by
`prepare_for_sleep()` just before the device enters deep sleep, and loaded by
`run_button_wake_path()` at the very start of a button-wake boot.

```cpp
struct RtcDisplaySnapshot {
  // Sensor values
  int   co2_ppm;          float pm25_ugm3;
  float temperature_c;    float humidity_pct;
  int   tvoc_index;       int   nox_index;
  float pressure_hpa;     float altitude_m;
  // Battery
  uint8_t battery_pct;  bool is_battery_charging;
  // Status flags & rendering settings
  bool gps_enabled;  bool gps_fix;  bool tracking_active;  bool ble_enabled;
  bool use_fahrenheit;  bool pm_use_usaqi;
};
```

Estimated size: ~40 bytes (well within the ~50 B budget; total RTC usage
~1.6 KB of the 8 KB available on ESP32-C5).

### Free functions

```cpp
// Save the last displayed state to RTC memory (go_display.cpp).
// Called from prepare_for_sleep() after the final display update.
void save_rtc_display_snapshot(const DisplayValues &values);

// Load the snapshot saved before the last deep sleep.
// Returns true and fills *snapshot_out when valid; false on first power-on.
bool load_rtc_display_snapshot(RtcDisplaySnapshot *snapshot_out);
```

Both functions are declared in `go_display.h` within the `#ifndef TEST_HOST`
guard, with inline no-op stubs in the `#else` branch so the orchestrator
compiles cleanly in host test builds.

The validity flag (`s_rtc_display_snapshot_valid`) is zero-initialized in RTC
memory on first power-on, so an uninitialized snapshot is never used. When
the snapshot is invalid, `build_wake_values()` fills the `DisplayValues` with
the default invalid sentinels, and the renderer shows dashes.

## Host-Compatible Types

`Screen`, `Metric`, `ListRow`, `DisplayValues`, and `RtcDisplaySnapshot` are
defined outside the `#ifndef TEST_HOST` guard and compile without ESP-IDF
headers. `DisplayService` and the snapshot free functions are
hardware-dependent and excluded from host builds (stubs provided).

### DisplayValues

Flat snapshot of everything needed to render one frame. Built by the UI Manager
on every update; the Display Service diffs against the previous snapshot
internally to select the refresh tier (Full/Fast/Partial).

Key points:
- TVOC/NOx are `int` (SGP41 algorithm index output, not raw resistance)
- Sensor readings come from channel A (`pm_a`, `temp_hum_a`)
- `ListRow::text` is `char[48]` (owned by struct, not a pointer)
- Invalid sentinels from `MeasuresInvalid`; `0xFF` for battery
- `ble_passkey` (`uint32_t`): 6-digit passkey for PairingPasskey screen

## Architecture

### Dual-Buffer Pattern

| Buffer | Size | Context | Purpose |
|---|---|---|---|
| `_render_buf[4096]` | 4096 B | Orchestrator thread | u8g2 render target |
| `_spi_buf[4096]` | 4096 B | Worker task | SPI transmit source |
| `_region_buf[3712]` | 3712 B | Worker task | Body region for partial writes |

On each `update()`, the render buffer is `memcpy`'d to the SPI buffer before
signaling the worker. The orchestrator can re-render freely without corrupting
an in-progress SPI transfer.

### Async Worker Task

The worker task waits on an RTOS task notification, then drives the SPI
hardware. The orchestrator signals frame-ready via `RTOS::task_notify_give()`.
A `volatile bool _worker_busy` flag allows the orchestrator to check if the
worker is available (`wait=false` returns false if busy; `wait=true` spins
until ready).

In the deferred-refresh mode (`defer_refresh=true`), `init()` itself signals
the worker with the initial full-refresh job before returning. This means the
first `RTOS::task_notify_give()` comes from the main task inside `init()`,
not from `update()`. The worker processes this exactly like any other full
refresh, acquiring and releasing the SPI bus normally.

### Display Driver

The SSD1680 display driver is file-local (anonymous namespace in
`go_display.cpp`). It follows the SPI protocol documented in
`UI-IMPLEMENTATION.md` sections 2.3-2.4:

- `driver_init()` — GPIO setup, SPI device attachment, DMA bounce buffer
- `driver_hw_init_full()` — Full SSD1680 init (SW reset, gate config, RAM window)
- `driver_set_basemap()` — Write frame to both RAM planes + full update trigger (`0xF7`)
- `driver_hw_init_fast()` — Fast refresh init: SW reset + temperature override trick (forces 100 °C OTP LUT for faster waveform)
- `driver_fast_write()` — Write full frame to both RAM planes (basemap coherence for subsequent partials)
- `driver_fast_commit()` — Trigger fast update (`0xC7` waveform, non-differential, no flash)
- `driver_part_begin/write_region/commit()` — Partial update protocol (`0xFF` waveform, differential)
- `driver_deep_sleep()` — SSD1680 deep sleep mode 1
- `driver_bus_acquire/release()` — Exclusive SPI bus access

## Three-Tier Display Refresh

The display uses three refresh tiers to balance image quality, speed, and
user experience. See `products/go/specs/display_refresh_tiers.md` for the
full design rationale.

### RefreshMode Enum

```cpp
enum class RefreshMode : uint8_t {
    Full,     ///< Full GC waveform (flash). Resets basemap in both RAM planes.
    Fast,     ///< Full-screen 0xC7 waveform (no flash). Temperature-override trick.
    Partial,  ///< Body-only differential 0xFF (no flash). Writes body region only.
};
```

### Refresh Tiers

| Tier | SSD1680 Waveform | Duration | Flash | When |
|---|---|---|---|---|
| **Full** | `0xF7` (GC, both RAM planes) | ~2–3 s | Yes | Init from deep sleep; anti-ghosting after 20 differential ops |
| **Fast** | `0xC7` (non-differential, 100 °C OTP LUT) | ~1–1.5 s | No | Transitions to/from PairingPasskey or Shutdown; navigable screen transitions with header change |
| **Partial** | `0xFF` (differential, body region only) | ~0.3–0.5 s | No | Menu navigation between navigable screens (header unchanged); same list screen updates |

### Decision Logic

The `update()` method selects the refresh mode using this priority:

1. **Full** — if `_diff_count >= max_partial_ops` (anti-ghosting, default 20)
2. **Partial** — if both screens are "navigable" and header unchanged, OR same list screen
3. **Fast** — everything else (fallback)

A screen is **navigable** if the user reaches it through normal menu
interaction: Home, MainMenu, Settings, SettingsChoice, TagList, Confirm,
About. PairingPasskey and Shutdown are not navigable — transitions involving
them always use Fast for clear visual indication.

All non-Shutdown screens share the same status bar at Y=0..17. Partial
updates write the body region only (Y=18..249, 232 px height, full 128 px
width), so the status bar is physically unchanged on the display. Header
changes during same-list-screen interactions are silently deferred; the
header self-corrects at the next Full refresh (anti-ghosting at count 20).

### Anti-Ghosting Counter

`_diff_count` counts all differential operations (both Fast and Partial).
Both waveform types accumulate ghosting equally on the GDEY0213B74 panel.
The counter resets to 0 on Full refresh and increments on Fast or Partial.
`Config::max_partial_ops` (default 20) controls the limit.

### Fast Refresh: Temperature Override

The Fast tier uses a vendor-documented technique for the GDEY0213B74 panel:

1. SW reset clears all SSD1680 registers
2. Read the built-in temperature sensor, then override the temperature
   register to 100 °C (`0x64`)
3. Reload the OTP LUT — the controller selects a faster, more aggressive
   waveform designed for high-temperature operation
4. Re-establish display geometry (data entry mode, RAM windows, cursor)
5. Write full frame to both RAM planes (0x24 and 0x26 — ensures basemap
   coherence for subsequent Partial refreshes)
6. Trigger with `0xC7` (skips LUT loading since it was done in step 3)

Writing both RAM planes ensures that after a Fast refresh, the basemap
(RAM 0x26) matches the displayed content. Subsequent Partial refreshes
(which diff 0x24 vs 0x26) produce correct results.

### Decision Matrix

| Condition | Refresh Mode |
|---|---|
| `diff_count >= max_partial_ops` | Full |
| Both navigable, header unchanged | Partial |
| Same list screen (any header state) | Partial |
| Screen transition involving PairingPasskey | Fast |
| Screen transition involving Shutdown | Fast |
| Navigable screen transition, header changed | Fast |

### Display Update Suppression

While the user is on a menu-navigation screen (MainMenu, Settings,
SettingsChoice, TagList, Confirm, About), background events — sensor data,
BLE connect/disconnect/auth/config writes, BMS charging-status changes, and
snackbar expiry — do **not** trigger display updates. Only user-initiated
events refresh the display on menu screens.

This policy is enforced by the orchestrator's
`request_background_display_update()`, which delegates to
`UIManager::is_on_menu_screen()` for the screen classification. Background
data is still cached internally and pushed to BLE clients; only the e-paper
refresh is suppressed to avoid unnecessary refreshes that interrupt menu
navigation. See `docs/orchestrator.md` for the full call-site classification.

## Rendering Pipeline

Frame assembly order:
1. Clear buffer to 0xFF (white)
2. Set draw color to 0 (black)
3. If Shutdown: `draw_shutdown()` + `draw_snackbar()`
4. Else: `draw_status_bar()` + screen-specific draw + `draw_snackbar()`

Screen dispatch:
- **Home:** Hero blocks (PM2.5, CO2 with dual-font labels centered via
  `u8g2_GetStrWidth()`) + 3-row grid (Temp/Humidity, TVOC/NOx or
  Min/Max, Pressure/Altitude or chart). Grid dividers span full 128 px;
  1st divider is always 2 px thick, 3rd is 2 px thick when chart is
  visible. Selection rects for PM2.5, CO2, Temp, and Humidity use full
  128 px width. Logo removed from home page (kept for display-off and
  shutdown only).
- **MainMenu:** Home screen (metric cleared to None) + overlay at y=162.
  The 2 px-thick 1st grid divider is preserved as the menu top border.
  Menu rows use full 128 px-wide selection rects.
- **Settings/SettingsChoice/TagList/Confirm/About:** Full-screen list with
  full 128 px-wide selection rects and vertically centered text. A
  separator line between the header rows (Exit/Back) and content rows
  uses a 2 px content offset to avoid touching.
- **PairingPasskey:** "Bluetooth Pairing" title + large 6-digit passkey + instruction
- **Shutdown:** "Powering off..." message + "See you soon" + logo

### Fonts

| Font | Usage |
|---|---|
| `u8g2_font_logisoso32_tr` | Hero section values (PM2.5, CO2) |
| `u8g2_font_logisoso16_tr` | Hero section metric name labels |
| `u8g2_font_helvR12_tr` | Hero section unit labels |
| `u8g2_font_helvR08_tr` | Grid cell labels, About page info text |
| `u8g2_font_helvB08_tf` | Grid cell values, About page title |
| `u8g2_font_6x10_tr` | Menu/list row text, logo text |
| `u8g2_font_siji_t_6x10` | Battery glyph, WiFi glyph, GPS glyph |
| `u8g2_font_open_iconic_all_1x_t` | Lock icon (glyph 0xCA) |
| `u8g2_font_open_iconic_thing_1x_t` | Unlock icon (glyph 0x44) |

### Value Formatting

| Metric | Rules |
|---|---|
| PM2.5 | <100: one decimal; >=100: integer; >999: "999+"; USAQI: EPA breakpoints |
| CO2 | Integer ppm; >9999: "9999+" |
| Temperature | One decimal, C or F; conversion: `F = C * 9/5 + 32` |
| Humidity | Integer percent |
| TVOC/NOx | Integer (index values) |
| Pressure | Integer hPa; >9999: "9999+ hPa" |
| Altitude | Integer meters; >9999: "9999+ m" |
| Invalid | "-" for any metric |
