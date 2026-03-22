# Display Service

Product-specific display service for AirGradient Go. Manages e-paper rendering
via u8g2, full/partial refresh decisions, and an async worker task for SPI
hardware refresh. The orchestrator calls the Display Service API which returns
immediately; the slow EPD refresh runs in a dedicated task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_display.h` | `DisplayService` class, `DisplayValues` struct, `Screen`/`Metric` enums |
| `products/go/main/go_display.cpp` | Rendering pipeline, refresh logic, worker task, display driver |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `u8g2` | `components/u8g2/` | Software rendering (fonts, primitives, framebuffer) |
| `go_types.h` | product | `OperatingMode`, `LockState` enums |
| `measures_types.h` | `airgradient-common` | `MeasuresInvalid` sentinel values |
| ESP-IDF SPI driver | ESP-IDF | `spi_device_*` API for EPD communication |
| ESP-IDF GPIO driver | ESP-IDF | CS, DC, RST, BUSY pin control |
| FreeRTOS task/notification | ESP-IDF | Worker task, frame-ready signaling |

## Display Hardware

**Panel:** Good Display GDEY0213B74, 128x250 px, 1-bit monochrome (SSD1680).
**Interface:** SPI half-duplex, mode 0, 4 MHz, CS managed manually.
**SPI bus:** Shared with NAND flash; uses `spi_device_acquire_bus()` for
exclusive access during display operations.

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
| `bus_acquire_timeout_ms` | `int` | 1000 | SPI bus acquire timeout |
| `task_stack_size` | `uint16_t` | 4096 | Worker task stack size |
| `task_priority` | `uint8_t` | 4 | Worker task FreeRTOS priority |
| `max_partial_ops` | `uint8_t` | 20 | Partial refresh limit before forced full |

### Methods

| Method | Blocking | Description |
|---|---|---|
| `init(initial)` | Yes | Init driver + u8g2, full refresh with initial values, start worker |
| `update(values, wait)` | No* | Render frame, signal worker. *wait=true blocks until worker ready |
| `update_sync(values)` | Yes | Render + SPI inline; for fast-path boot without worker |
| `clear()` | Yes | Clear display to white via full refresh |
| `deep_sleep()` | Yes | Put SSD1680 into deep sleep mode |
| `stop()` | Yes | Stop worker task; call before ESP deep sleep |

## Host-Compatible Types

`Screen`, `Metric`, `ListRow`, and `DisplayValues` are defined outside the
`#ifndef TEST_HOST` guard and compile without ESP-IDF headers. The
`DisplayService` class is hardware-dependent and excluded from host builds.

### DisplayValues

Flat snapshot of everything needed to render one frame. Built by the UI Manager
on every update; the Display Service diffs against the previous snapshot
internally to decide partial vs full refresh.

Key points:
- TVOC/NOx are `int` (SGP41 algorithm index output, not raw resistance)
- Sensor readings come from channel A (`pm_a`, `temp_hum_a`)
- `ListRow::text` is `char[48]` (owned by struct, not a pointer)
- Invalid sentinels from `MeasuresInvalid`; `0xFF` for clock/battery

## Architecture

### Dual-Buffer Pattern

| Buffer | Size | Context | Purpose |
|---|---|---|---|
| `_render_buf[4096]` | 4096 B | Orchestrator thread | u8g2 render target |
| `_spi_buf[4096]` | 4096 B | Worker task | SPI transmit source |
| `_region_buf[3680]` | 3680 B | Worker task | Body region for partial writes |

On each `update()`, the render buffer is `memcpy`'d to the SPI buffer before
signaling the worker. The orchestrator can re-render freely without corrupting
an in-progress SPI transfer.

### Async Worker Task

The worker task waits on a FreeRTOS task notification, then drives the SPI
hardware. The orchestrator signals frame-ready via `xTaskNotifyGive()`.
A `volatile bool _worker_busy` flag allows the orchestrator to check if the
worker is available (wait=false returns false if busy; wait=true spins until
ready).

### Display Driver

The SSD1680 display driver is file-local (anonymous namespace in
`go_display.cpp`). It follows the SPI protocol documented in
`UI-IMPLEMENTATION.md` sections 2.3-2.4:

- `driver_init()` — GPIO setup, SPI device attachment, DMA bounce buffer
- `driver_hw_init_full()` — Full SSD1680 init (SW reset, gate config, RAM window)
- `driver_set_basemap()` — Write frame to both RAM planes + full update trigger
- `driver_part_begin/write_region/commit()` — Partial update protocol
- `driver_deep_sleep()` — SSD1680 deep sleep mode 1
- `driver_bus_acquire/release()` — Exclusive SPI bus access

## Full vs Partial Refresh

A partial refresh is used only when:
1. Both previous and current screen are "home-like" (Home or MainMenu)
2. No status bar fields have changed (time, battery, BLE, WiFi, GPS, etc.)
3. Partial op counter has not reached `max_partial_ops` (default: 20)

Otherwise a full refresh is forced. The partial op counter resets on every full
refresh to prevent e-paper ghosting.

Partial updates write the body region only (Y=20..249, 230 px height, full
128 px width). The status bar is never partially updated.

## Rendering Pipeline

Frame assembly order:
1. Clear buffer to 0xFF (white)
2. Set draw color to 0 (black)
3. If Shutdown: `draw_shutdown()` + `draw_snackbar()`
4. Else: `draw_status_bar()` + screen-specific draw + `draw_snackbar()`

Screen dispatch:
- **Home:** Hero blocks (PM2.5, CO2) + secondary grid + chart or logo
- **MainMenu:** Home screen + half-screen menu overlay
- **Settings/SettingsChoice/TagList/Confirm/About:** Full-screen list

### Fonts

| Font | Usage |
|---|---|
| `u8g2_font_6x10_tr` | Labels, status bar, menu items, logo |
| `u8g2_font_10x20_tn` | Large numeric values (PM2.5, CO2 hero blocks) |
| `u8g2_font_siji_t_6x10` | Battery icon glyphs |

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
