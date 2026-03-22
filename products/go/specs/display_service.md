# Display Service — Implementation Spec

Product-specific display service for AirGradient Go. Manages e-paper rendering
via u8g2, full/partial refresh decisions, and an async worker task for SPI
hardware refresh. The orchestrator calls the Display Service API which returns
immediately; the slow EPD refresh runs in a dedicated task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_display.h` | `DisplayService` class, `DisplayValues` struct, `Screen`/`Metric` enums |
| `products/go/main/go_display.cpp` | Rendering pipeline, refresh logic, worker task, display driver |

All display code lives in a single translation unit. The display driver (raw SPI
commands to the SSD1680 controller) is file-local to `go_display.cpp` in an
anonymous namespace. The rest of the system only sees the `DisplayService` class.

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `u8g2` | `components/u8g2/` | Software rendering (fonts, primitives, framebuffer) |
| `go_types.h` | product | `OperatingMode`, `LockState` enums |
| `measures_types.h` | `airgradient-common` | `MeasuresInvalid` sentinel values |
| ESP-IDF SPI driver | ESP-IDF | `spi_device_*` API for EPD communication |
| ESP-IDF GPIO driver | ESP-IDF | CS, DC, RST, BUSY pin control |
| FreeRTOS task/semaphore | ESP-IDF | Worker task, frame-ready signaling |

## Display Hardware

### Panel

- **Model**: Good Display GDEY0213B74
- **Controller**: SSD1680
- **Resolution**: 128 x 250 pixels, 1-bit monochrome (black/white)
- **Frame buffer**: 4000 bytes (128 * 250 / 8)
- **Interface**: SPI half-duplex, mode 0

### Pin Assignments

All pins come from `board_config.h`. The following table shows expected
assignments (to be confirmed when `board_config.h` is populated):

| Signal | Expected GPIO | Direction | Purpose |
|---|---|---|---|
| SPI MOSI | TBD | Output | Serial data to display |
| SPI MISO | TBD | Input | Unused for EPD, shared bus |
| SPI SCLK | TBD | Output | SPI clock |
| EPD CS | TBD | Output | Chip select (active low, manual toggle) |
| EPD DC | TBD | Output | Data/Command select (0=cmd, 1=data) |
| EPD RST | TBD | Output | Hardware reset (active low pulse) |
| EPD BUSY | TBD | Input | Busy status (1=busy, 0=idle) |

**SPI bus**: Shared with NAND flash storage. The Display Service acquires
exclusive SPI bus access via `spi_device_acquire_bus()` before multi-command
sequences and releases with `spi_device_release_bus()`.

**SPI configuration**: 4 MHz clock, mode 0, half-duplex, CS managed manually
(not by ESP-IDF SPI driver). Max transfer size: 4096 bytes.

### Display Driver (file-local)

The display driver is implemented as file-local functions in `go_display.cpp`
within an anonymous namespace. It follows the SSD1680 command protocol from the
reference implementation (see `UI-IMPLEMENTATION.md` sections 2.3-2.4 for the
full SPI protocol, initialization sequence, and partial update protocol).

Key driver functions:

| Function | Purpose |
|---|---|
| `driver_init(config)` | Initialize GPIOs, attach SPI device, allocate DMA bounce buffer |
| `driver_hw_init_full()` | Full SSD1680 init sequence (SW reset, gate config, RAM window) |
| `driver_set_basemap(data)` | Write frame to both RAM planes + trigger full update |
| `driver_part_begin()` | Prepare controller for partial update mode |
| `driver_part_write_region(x, y, data, h, w)` | Write rectangular region to partial RAM |
| `driver_part_commit()` | Trigger partial refresh |
| `driver_deep_sleep()` | Put SSD1680 into deep sleep mode |
| `driver_bus_acquire()` | Acquire exclusive SPI bus access |
| `driver_bus_release()` | Release SPI bus |

The coordinate system, Y-inversion, and command sequences are identical to the
reference implementation documented in `UI-IMPLEMENTATION.md` section 2.3.

## Enums

```cpp
enum class Screen : uint8_t {
    Home,
    MainMenu,
    Settings,
    SettingsChoice,
    TagList,
    About,
    Confirm,
    Shutdown,
};

enum class Metric : uint8_t {
    None,
    Pm25,
    Co2,
    Temp,
    Humidity,
    Tvoc,
    Nox,
};
```

These enums are defined in `go_display.h` and used by both the Display Service
and the UI Manager.

## DisplayValues Struct

The complete interface between the UI Manager and the Display Service. A flat
snapshot of everything needed to render one frame. The caller builds a full
snapshot on every update; the Display Service diffs against the previous snapshot
internally to decide partial vs full refresh.

```cpp
static constexpr uint8_t MAX_LIST_ROWS = 9;

struct ListRow {
    char text[48];
    bool disabled = false;
};

struct DisplayValues {
    // --- Sensor readings ---
    // Source: Measures.pm_a, .temp_hum_a, .co2, .tvoc_nox (channel A)
    int co2_ppm              = MeasuresInvalid::CO2;
    float pm25_ugm3          = MeasuresInvalid::PM;
    float temperature_c      = MeasuresInvalid::TEMPERATURE;
    float humidity_pct       = MeasuresInvalid::HUMIDITY;
    int tvoc_index           = MeasuresInvalid::TVOC;
    int nox_index            = MeasuresInvalid::NOX;
    float pressure_hpa       = MeasuresInvalid::PM;   // no dedicated sentinel
    float altitude_m         = MeasuresInvalid::PM;

    // --- Clock (from GPS) ---
    uint8_t hour             = 0xFF;    // 0xFF = no data
    uint8_t minute           = 0xFF;

    // --- Battery ---
    uint8_t battery_pct      = 0xFF;    // 0xFF = no data
    bool is_battery_charging = false;

    // --- Status flags ---
    bool locked              = true;
    bool ble_enabled         = false;
    bool ble_connected       = false;
    bool wifi_enabled        = false;
    bool gps_enabled         = true;
    bool gps_fix             = false;
    bool tracking_active     = false;
    bool display_off         = false;
    bool use_fahrenheit      = false;
    bool pm_use_usaqi        = false;

    // --- Screen navigation ---
    Screen screen            = Screen::Home;
    Metric active_metric     = Metric::None;

    // --- List/menu content ---
    ListRow rows[MAX_LIST_ROWS] = {};
    uint8_t row_count        = 0;
    uint8_t selected_row     = 0;
    bool show_separator_after_back = false;

    // --- About screen ---
    const char *about_title    = nullptr;
    const char *about_firmware = nullptr;
    const char *about_serial   = nullptr;
    const char *about_hardware = nullptr;

    // --- Chart data ---
    const float *chart_samples = nullptr;
    uint8_t chart_count      = 0;
    float chart_min          = 0.0f;
    float chart_max          = 0.0f;

    // --- Snackbar ---
    const char *snackbar_text = nullptr;
};
```

**Design notes**:

- `ListRow::text` uses a fixed char array rather than `const char *` because the
  UI Manager populates rows with formatted strings (e.g. `"Units: C"`) built
  from temporary buffers. Fixed arrays ensure the data is owned by the struct
  and survives the formatting scope.
- TVOC and NOx are `int` (matching `TVOCNOxData::tvoc_index` /
  `TVOCNOxData::nox_index`). The display uses the SGP41 algorithm index output,
  not the raw resistance values.
- Sensor readings come from channel A (`pm_a`, `temp_hum_a`). AGo has a single
  PM sensor and a single temp/humidity sensor, both wired to channel A.
- Pressure and altitude use `MeasuresInvalid::PM` as sentinel because
  `MeasuresInvalid` has no dedicated pressure/altitude constants. These values
  come from GPS (barometric altitude) or are omitted on platforms without a
  pressure sensor.

## Screen Layout

Screen layout and pixel geometry are documented in `UI-IMPLEMENTATION.md`
sections 4.1-4.9. The rendering implementation follows those layouts exactly.

### Display Regions

```
Y=0   +----------------------------+
      |       Status Bar           |  H=19px
Y=19  +----------------------------+  1px divider line
Y=20  |                            |
      |       Body Content         |  H=230px (varies by screen)
      |                            |
Y=250 +----------------------------+
```

Content width: 122 px. Full buffer width: 128 px.

### Home Screen Summary

| Zone | Y Range | Content |
|---|---|---|
| PM2.5 hero | 27-70 | Label + large value |
| CO2 hero | 74-120 | Label + large value |
| Main divider | 127 | Double line |
| Grid row 1 | 134-161 | Temp / Humidity |
| Grid row 2 | 163-190 | TVOC / NOx |
| Grid row 3 | 192-219 | Pressure+Altitude (normal) or Min+Max (chart mode) |
| Bottom | 221-249 | Logo (normal) or chart (metric selected) |

When a metric is selected, its area is inverted (white on black) and the bottom
section shows a chart with min/max stats instead of pressure/altitude/logo.

Refer to `UI-IMPLEMENTATION.md` for exact pixel coordinates, menu overlay
geometry, status bar element positions, snackbar layout, and shutdown screen.

## u8g2 Setup

u8g2 is used as a software-only renderer into RAM. No u8g2 display callbacks
drive hardware. The display driver handles all SPI communication.

```cpp
// Virtual display callback -- provides geometry info only
u8g2_SetupDisplay(&_u8g2, u8x8_d_epd_128x250_cb,
                  u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);

// Buffer: 16 bytes/row * 32 tiles = 4096 bytes
u8g2_SetupBuffer(&_u8g2, _render_buf, BUF_TILE_HEIGHT,
                 u8g2_ll_hvline_horizontal_right_lsb, U8G2_MIRROR);

// Transparent font mode
u8g2_SetFontMode(&_u8g2, 1);
```

**Buffer**: 4096 bytes (16 bytes/row * 256 px, oversized for the 250 px panel).
**Draw color**: 0 = black on white background (EPD convention: 0xFF = white).
**Pixel format**: horizontal bytes, right-to-left LSB, with X mirroring.

### Fonts

| Font | Usage |
|---|---|
| `u8g2_font_6x10_tr` | Labels, status bar, menu items, logo text |
| `u8g2_font_10x20_tn` | Large numeric values (PM2.5, CO2 hero blocks) |
| `u8g2_font_siji_t_6x10` | Battery icon glyphs |

All three are compiled in `components/u8g2/fonts/`.

## Rendering Pipeline

### Per-Screen Draw Functions

| Function | Screen(s) | Content |
|---|---|---|
| `draw_status_bar(v)` | All except Shutdown | Lock, BLE, WiFi, GPS, tracking, battery |
| `draw_home(v)` | Home | PM hero, CO2 hero, secondary grid, chart or logo |
| `draw_menu_overlay(v)` | MainMenu | Half-screen menu over home screen body |
| `draw_full_screen_list(v)` | Settings, SettingsChoice, TagList, About, Confirm | Full-screen list |
| `draw_snackbar(v)` | Any | Bottom notification bar |
| `draw_shutdown()` | Shutdown | "Powering off..." + logo |

### Frame Assembly

```
render_frame(values):
    clear _render_buf to 0xFF (white)
    set u8g2 draw color to 0 (black)

    if screen == Shutdown:
        draw_shutdown()
        draw_snackbar(values)
        return

    draw_status_bar(values)

    switch screen:
        Home:        draw_home(values)
        MainMenu:    draw_home(values) then draw_menu_overlay(values)
        others:      draw_full_screen_list(values)

    draw_snackbar(values)
```

### Value Formatting

Inline during rendering using `use_fahrenheit` and `pm_use_usaqi` flags.
Rules from `UI-IMPLEMENTATION.md` section 11:

| Metric | Format |
|---|---|
| PM2.5 | < 100: one decimal. >= 100: integer. > 999: "999+". USAQI: EPA breakpoints. |
| CO2 | Integer ppm. > 9999: "9999+" |
| Temperature | One decimal, C or F. `F = C * 9/5 + 32` |
| Humidity | Integer percent |
| TVOC/NOx | Integer (index values from SGP41) |
| Pressure | Integer hPa. > 9999: "9999+ hPa" |
| Altitude | Integer meters. > 9999: "9999+ m" |
| Invalid | "-" for any metric |

## Full vs Partial Refresh Logic

### Decision Flow

```
update(values):
    1. header_changed = is_header_changed(values, _prev_values)
    2. can_partial = (prev screen is Home or MainMenu)
                     AND (new screen is Home or MainMenu)
                     AND NOT header_changed
    3. render_frame(values) into _render_buf
    4. if NOT can_partial OR _partial_count >= MAX_PARTIAL_OPS:
         -> full refresh (resets _partial_count to 0)
       else:
         -> partial refresh (increments _partial_count)
    5. _prev_values = values
```

### Header Change Detection

Compares these fields between previous and current values:

- `hour`, `minute`
- `battery_pct`, `is_battery_charging`
- `locked`
- `ble_enabled`, `ble_connected`, `wifi_enabled`
- `gps_enabled`, `gps_fix`
- `tracking_active`

Any change forces a full refresh.

### Partial Op Limit

`MAX_PARTIAL_OPS = 20`. After 20 consecutive partial updates, the next is
forced full to prevent e-paper ghosting.

### Refresh Scope

- **Full refresh**: entire 128x250 frame. Written to both SSD1680 RAM planes.
- **Partial refresh**: body region only (Y=20 to Y=249, full width, 230 px).
  Status bar is never partially updated.

## Async Worker Task

### Architecture

```
Orchestrator thread:                    Worker task:

  display.update(values)                  loop:
    render_frame(values) [fast]             wait for frame-ready signal
    memcpy _render_buf -> _spi_buf          acquire SPI bus
    set refresh type (full/partial)         if full: hw_init + set_basemap(_spi_buf)
    signal frame-ready                      if partial: extract region from _spi_buf
    return immediately                                  part_begin + write + commit
                                            release SPI bus
                                            signal frame-done
```

### Buffer Strategy

Two framebuffers decouple rendering from transmission:

| Buffer | Size | Context | Purpose |
|---|---|---|---|
| `_render_buf[4096]` | 4096 bytes | Orchestrator | u8g2 render target |
| `_spi_buf[4096]` | 4096 bytes | Worker task | SPI transmit source |

On each `update()`, the render buffer is `memcpy`'d to the SPI buffer before
signaling the worker. The orchestrator can re-render freely without corrupting
an in-progress SPI transfer. Total cost: +4 KB RAM.

Partial updates also use a region buffer:

| Buffer | Size | Purpose |
|---|---|---|
| `_region_buf[3680]` | 3680 bytes | Body-region extraction for partial SPI writes |

The region buffer is populated from `_spi_buf` by the worker task.

### Signaling

- **Frame-ready**: task notification from orchestrator to worker
- **Frame-done**: task notification from worker (checked by orchestrator)

### Busy Handling

The `update()` method accepts a `wait` parameter:

```cpp
bool update(const DisplayValues &values, bool wait = false);
```

- `wait=false` (default): If worker is still busy, return `false` (frame
  skipped). Good for periodic locked-mode refreshes.
- `wait=true`: Block until worker finishes previous refresh, then submit. Used
  for interactive updates where visual responsiveness matters.

Returns `true` if the frame was submitted, `false` if skipped.

## DisplayService Class

```cpp
class DisplayService {
  public:
    struct Config {
        // SPI
        spi_host_device_t spi_host;
        int pin_cs;
        int pin_dc;
        int pin_rst;
        int pin_busy;
        int clock_hz             = 4000000;
        int bus_acquire_timeout_ms = 1000;

        // Worker task
        uint16_t task_stack_size = 4096;
        uint8_t task_priority    = 4;

        // Refresh limits
        uint8_t max_partial_ops  = 20;
    };

    explicit DisplayService(const Config &config);

    /// Initialize display hardware and start worker task.
    /// Performs a full refresh with the initial values.
    bool init(const DisplayValues &initial);

    /// Submit a new frame for display.
    /// Renders into framebuffer (fast), then signals worker task.
    /// wait=false and worker busy: returns false (skipped).
    /// wait=true: blocks until worker finishes previous refresh.
    bool update(const DisplayValues &values, bool wait = false);

    /// Synchronous one-shot update for fast-path boot.
    /// Renders and drives SPI inline (blocking). Does not use worker task.
    void update_sync(const DisplayValues &values);

    /// Clear display to white (full refresh). Blocking.
    void clear();

    /// Put EPD controller into deep sleep mode.
    void deep_sleep();

    /// Stop worker task. Call before entering ESP deep sleep.
    void stop();

  private:
    Config _config;

    // u8g2 instance and render buffer
    u8g2_t _u8g2;
    uint8_t _render_buf[4096];

    // SPI transmit buffer (owned by worker after signal)
    uint8_t _spi_buf[4096];
    uint8_t _region_buf[3680];

    // Refresh state
    DisplayValues _prev_values;
    uint8_t _partial_count = 0;
    bool _pending_full     = false;

    // Worker task
    TaskHandle_t _task_handle = nullptr;
    volatile bool _running    = false;

    // Render methods
    void render_frame(const DisplayValues &v);
    bool is_header_changed(const DisplayValues &a, const DisplayValues &b) const;

    void draw_status_bar(const DisplayValues &v);
    void draw_home(const DisplayValues &v);
    void draw_menu_overlay(const DisplayValues &v);
    void draw_full_screen_list(const DisplayValues &v);
    void draw_snackbar(const DisplayValues &v);
    void draw_shutdown();
    void draw_chart(const DisplayValues &v);

    // Worker
    static void worker_entry(void *arg);
    void worker_loop();
};
```

## Fast-Path Boot Support

For deep sleep timer-wake (see `ARCHITECTURE.md` section 7.4), the Display
Service supports a synchronous update without the worker task:

```cpp
void update_sync(const DisplayValues &values);
```

Renders into `_render_buf`, then directly drives SPI in the calling context.
Blocking. Used only during fast-path where no event loop is running.

## Required Settings (not yet in GoSettings)

The Display Service reads formatting flags from `DisplayValues`, populated by
the UI Manager from settings. These settings are needed but not yet defined in
`go_settings.h`:

| Setting | Type | Default | Purpose |
|---|---|---|---|
| `use_fahrenheit` | bool | false | Temperature display unit |
| `pm_use_usaqi` | bool | false | PM display format |

These should be added to `GoSettings` in a future settings.md update.

## Testability

For host testing under `TEST_HOST`:

- Rendering logic (all `draw_*` functions) operates on the u8g2 framebuffer.
  u8g2 compiles natively. Tests can render frames and inspect buffer contents.
- `is_header_changed()` is a pure function on two `DisplayValues` structs.
- Display driver and worker task are hardware-dependent; excluded from host tests.
- `DisplayValues`, `Screen`, `Metric`, and `ListRow` are plain data types with
  no platform dependency.

Test cases:
- Header change detection: each status field triggers full refresh independently
- Partial op counter: resets after max_partial_ops
- Screen transition: non-Home transitions force full refresh
- Value formatting: PM, CO2, temp edge cases (overflow, invalid, unit conversion)
