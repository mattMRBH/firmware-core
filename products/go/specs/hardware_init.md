# Hardware Initialization — Implementation Spec

Boot path selection and hardware initialization for AirGradient Go. All
initialization logic lives in `main.cpp` — no BSP layer. AGo has a single
board revision, so a hardware abstraction layer between the product and the
board adds indirection without benefit.

## Files

| File | Purpose |
|---|---|
| `products/go/main/main.cpp` | `app_main()`, boot path selection, hardware init, fast-path boot |
| `products/go/main/board_config.h` | Pin assignments and peripheral constants |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| ESP-IDF I2C master driver | `esp_driver_i2c` | I2C bus for sensors, BMS, touch |
| ESP-IDF SPI master driver | `esp_driver_spi` | SPI bus for display and NAND |
| ESP-IDF NVS | `nvs_flash` | Settings persistence |
| `gpio::Hal` | `airgradient-gpio` | GPIO configuration (power enables, buttons) |
| `AirgradientUART` | `airgradient-serial` | UART for GPS and serial sensors |
| `SHT40` | `airgradient-sensors` | Temperature + humidity driver |
| `S8` / `Sunlight` | `airgradient-sensors` | CO2 driver (exact model TBD at board bring-up) |
| `PMS5003` | `airgradient-sensors` | PM driver |
| `SGP41` | `airgradient-sensors` | TVOC + NOx driver |
| `BQ25XX` | `airgradient-bms` | BMS charger/PMIC driver |
| `CAP1203` | `airgradient-touch` | Capacitive touch driver |
| `NmeaGps` | `airgradient-gps` | GPS NMEA parser |
| `NandStorage` | `airgradient-nand-storage` | SPI NAND flash storage |
| `PayloadCache` | `airgradient-payload-cache` | Temporary measurement cache (RTC-backed) |
| `ConfigStore` | `airgradient-config` | NVS key-value configuration |
| `SensorManager` | `airgradient-sensors` | Sensor orchestration / averaging |
| All product services | product (`go_*.h`) | See ARCHITECTURE.md §8 |
| `Orchestrator` | product (`go_orchestrator.h`) | Central event loop |
| `load_rtc_app_state()` | product (`go_power.h` or `go_types.h`) | Free function to read RTC state before PowerService construction |

## board_config.h

Pin assignments and peripheral constants for the AGo board. All values are
`inline constexpr`. Actual GPIO numbers are TBD until the hardware schematic
is finalized.

```cpp
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/uart.h>

// --- I2C bus ---
inline constexpr int PIN_I2C_SDA = TBD;
inline constexpr int PIN_I2C_SCL = TBD;

// --- SPI (display — SSD1680 e-paper) ---
inline constexpr spi_host_device_t SPI_HOST_DISPLAY = SPI2_HOST;
inline constexpr int PIN_DISPLAY_MOSI = TBD;
inline constexpr int PIN_DISPLAY_CLK  = TBD;
inline constexpr int PIN_DISPLAY_CS   = TBD;
inline constexpr int PIN_DISPLAY_DC   = TBD;
inline constexpr int PIN_DISPLAY_RST  = TBD;
inline constexpr int PIN_DISPLAY_BUSY = TBD;

// --- SPI (NAND flash) ---
// May share the SPI host with display or use a separate host.
inline constexpr spi_host_device_t SPI_HOST_NAND = SPI3_HOST;
inline constexpr int PIN_NAND_MOSI = TBD;
inline constexpr int PIN_NAND_MISO = TBD;
inline constexpr int PIN_NAND_CLK  = TBD;
inline constexpr int PIN_NAND_CS   = TBD;

// --- UART (GPS — NmeaGps) ---
inline constexpr uart_port_t UART_PORT_GPS = UART_NUM_1;
inline constexpr int PIN_GPS_RX   = TBD;
inline constexpr int PIN_GPS_TX   = TBD;
inline constexpr int GPS_BAUD     = 9600;

// --- UART (CO2 sensor — S8 or Sunlight) ---
inline constexpr uart_port_t UART_PORT_CO2 = UART_NUM_0;
inline constexpr int PIN_CO2_RX   = TBD;
inline constexpr int PIN_CO2_TX   = TBD;
inline constexpr int CO2_BAUD     = 9600;

// --- I2C device addresses ---
inline constexpr uint8_t I2C_ADDR_SHT40   = 0x44;
inline constexpr uint8_t I2C_ADDR_SGP41   = 0x59;
inline constexpr uint8_t I2C_ADDR_BMS     = TBD;  // BQ25672 or BQ25798
inline constexpr uint8_t I2C_ADDR_CAP1203 = 0x28;

// --- Physical buttons ---
inline constexpr int PIN_BUTTON_POWER = TBD;
inline constexpr int PIN_BUTTON_BOOT  = TBD;

// --- Capacitive touch interrupt ---
inline constexpr int PIN_CAP_INT       = TBD;
inline constexpr int TOUCH_DELTA_SENSE = 0;  // max sensitivity

// --- Power enables (populated from schematic) ---
// inline constexpr int PIN_PMS_ENABLE = TBD;    // -1 if always on
// ... other enables TBD

#endif  // BOARD_CONFIG_H
```

## Boot Path Selection

`app_main()` determines the boot path before any peripheral initialization:

```
app_main():
    WakeCause cause = PowerService::get_wake_cause()

    if cause == WakeCause::Timer:
        RtcAppState state = load_rtc_app_state()
        if PowerService::is_fast_path_wake(cause, state):
            run_fast_path(state)
            // never returns

    // Full boot: fresh power-on or button wake
    run_full_boot(cause)
    // never returns
```

Three paths:

| Wake Cause | Lock State | Path |
|---|---|---|
| `PowerOn` | — | Full boot with default state |
| `Timer` | `Locked` | Fast-path: minimal init → measure → display → sleep |
| `Timer` | `Unlocked` | Full boot (should not normally happen) |
| `Button` | — | Full boot, restore state from RTC, unlock |

### load_rtc_app_state()

Free function that reads the `RTC_DATA_ATTR` state variable directly.
Needed because the fast-path decision must happen before `PowerService` is
constructed (which requires I2C bus and BMS driver).

```cpp
/// Read RtcAppState from RTC memory. Returns default state if no valid
/// state has been saved. No dependencies — safe to call early in app_main.
RtcAppState load_rtc_app_state();
```

This function accesses the same `RTC_DATA_ATTR` static that
`PowerService::save_state()` / `load_state()` use. Under `TEST_HOST`,
`RTC_DATA_ATTR` is defined away and the variable becomes a regular static.

## Full Initialization Sequence

`run_full_boot(cause)` initializes all hardware and services, then hands
control to the Orchestrator. All objects are constructed on the stack in
`app_main()` — since `Orchestrator::run()` never returns, they live for the
duration of the program.

```
run_full_boot(cause):

    // --- 1. NVS ---
    init_nvs()

    // --- 2. Settings ---
    ConfigStore config_store
    config_store.init()
    GoSettings settings = load_go_settings(config_store)

    // --- 3. GPIO (power enables, initial levels) ---
    init_gpio()
    RTOS::delay_ms(100)                        // settling time

    // --- 4. I2C bus ---
    i2c_bus = init_i2c_bus()
    RTOS::delay_ms(100)                        // settling time

    // --- 5. SPI bus(es) ---
    init_spi_buses()

    // --- 6. Sensor drivers ---
    SHT40 sht40(i2c_bus, I2C_ADDR_SHT40)
    SGP41 sgp41(i2c_bus, I2C_ADDR_SGP41)

    AirgradientUART co2_serial(UART_PORT_CO2, PIN_CO2_RX, PIN_CO2_TX)
    co2_serial.begin(CO2_BAUD)
    S8 co2(co2_serial)                         // or Sunlight

    AirgradientUART pm_serial(...)             // pins TBD
    pm_serial.begin(...)
    PMS5003 pms(pm_serial)

    sht40.init()
    sgp41.init()
    co2.init()

    // --- 7. Sensors struct + SensorManager ---
    Sensors sensors{}
    sensors.temp_hum = &sht40
    sensors.co2      = &co2
    sensors.pms_a    = &pms
    sensors.tvoc_nox = &sgp41
    // sensors.pms_b  = nullptr (default)
    // sensors.o3_no2 = nullptr (default)

    SensorManager sensor_manager(sensors)

    // --- 8. BMS ---
    BQ25XX bms(i2c_bus, I2C_ADDR_BMS)
    bms.init()

    // --- 9. GPS ---
    AirgradientUART gps_serial(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX)
    gps_serial.begin(GPS_BAUD)
    NmeaGps nmea_gps(gps_serial)

    // --- 10. Touch ---
    CAP1203 touch(i2c_bus, I2C_ADDR_CAP1203)
    touch.init()

    // --- 11. Storage ---
    PayloadCache cache(...)                    // RTC-backed
    NandStorage nand(nand_config)

    StorageService storage(cache, nand)
    storage.restore_cache()                    // recover RTC cache after deep sleep
    storage.init()                             // mount NAND

    // --- 12. Event queue ---
    RtosQueueHandle event_queue =
        RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event))

    // --- 13. Construct services ---
    SensorProducer sensor_producer(sensor_manager, event_queue, {
        .task_stack_size = 4096,
        .task_priority   = 5,
    })

    GpsService gps_service(nmea_gps, event_queue, {
        .baud_rate           = GPS_BAUD,
        .posting_interval_ms = settings.gps_interval_seconds * 1000,
        .task_stack_size     = 4096,
        .task_priority       = 5,
    })

    InputService input_service(touch, gpio::native::hal, event_queue, {
        .pin_cap_int       = PIN_CAP_INT,
        .pin_button_power  = PIN_BUTTON_POWER,
        .pin_button_boot   = PIN_BUTTON_BOOT,
    })

    DisplayService display_service({
        .spi_host = SPI_HOST_DISPLAY,
        .pin_cs   = PIN_DISPLAY_CS,
        .pin_dc   = PIN_DISPLAY_DC,
        .pin_rst  = PIN_DISPLAY_RST,
        .pin_busy = PIN_DISPLAY_BUSY,
    })

    PowerService power_service(bms, gpio::native::hal, {
        .pin_wake_button_power = PIN_BUTTON_POWER,
        .pin_wake_button_boot  = PIN_BUTTON_BOOT,
    })

    UIManager ui_manager({
        .firmware_version = FIRMWARE_VERSION,
        .serial_number    = get_serial_number(),
    })

    // --- 14. Init display (shows initial empty dashboard) ---
    DisplayValues initial{}
    display_service.init(initial)

    // --- 15. Start producer tasks ---
    sensor_producer.start()
    gps_service.start()
    input_service.start()

    // --- 16. Construct and run orchestrator ---
    Orchestrator::Services services = {
        .sensor_producer = sensor_producer,
        .gps_service     = gps_service,
        .input_service   = input_service,
        .display_service = display_service,
        .storage_service = storage,
        .power_service   = power_service,
        .ui_manager      = ui_manager,
    }

    Orchestrator orchestrator(event_queue, services, settings, config_store)
    orchestrator.init(cause)
    orchestrator.run()          // never returns
```

### Initialization Order Rationale

| # | What | Why this order |
|---|---|---|
| 1 | NVS | Settings depend on NVS |
| 2 | Settings | Configuration needed for service construction |
| 3 | GPIO | Power enables must be set before drivers access peripherals |
| 4 | I2C | Sensors, BMS, touch all share the I2C bus |
| 5 | SPI | Display and NAND on SPI |
| 6–7 | Sensor drivers + SensorManager | Must exist before SensorProducer |
| 8 | BMS | Independent of sensors; needed for PowerService |
| 9 | GPS | Independent; UART must be initialized before GpsService |
| 10 | Touch | I2C; needed for InputService |
| 11 | Storage | NAND mount + cache restore before orchestrator runs |
| 12 | Event queue | All producer services need the queue handle |
| 13 | Services | Depend on all drivers and infrastructure above |
| 14 | Display | Show initial screen before event loop |
| 15 | Start tasks | Services ready to produce events |
| 16 | Orchestrator | Last — owns the event loop |

## Fast-Path Boot

Timer wake while locked. Minimal initialization — no event loop, no
producer tasks, no input handling. Goal: measure, display, sleep — as fast
as possible to conserve battery.

```
run_fast_path(state):

    // --- 0. NVS + settings (for sleep duration, GPS mode) ---
    init_nvs()
    ConfigStore config_store
    config_store.init()
    GoSettings settings = load_go_settings(config_store)

    // --- 1. GPIO (sensor power enables) ---
    init_gpio()
    RTOS::delay_ms(100)

    // --- 2. I2C bus ---
    i2c_bus = init_i2c_bus()
    RTOS::delay_ms(100)

    // --- 3. Sensor drivers (same construction as full boot) ---
    ... construct SHT40, SGP41, CO2 serial + driver, PM serial + driver ...
    ... init each driver ...

    Sensors sensors{...}
    SensorManager sensor_manager(sensors)

    // --- 4. One-shot measurement (blocking, single iteration) ---
    MeasuresAGo measures = sensor_manager.start_measures(1)

    // --- 5. One-shot GPS (if tracking + GPS active) ---
    GpsData gps{}
    bool gps_active = (settings.gps_mode == GpsMode::AlwaysOn)
                   || (settings.gps_mode == GpsMode::OnWhenTracking
                       && state.tracking_active)
    if state.tracking_active and gps_active:
        AirgradientUART gps_serial(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX)
        NmeaGps nmea_gps(gps_serial)
        gps = gps_read_once(nmea_gps, GPS_BAUD, 2000)

    // --- 6. Storage ---
    PayloadCache cache(...)
    NandStorage nand(nand_config)
    StorageService storage(cache, nand)
    storage.restore_cache()
    storage.init()

    storage.cache_measurement(measures)

    if state.tracking_active:
        storage.start_route(state.tracking_session_id)
        RoutePoint point = { time(nullptr), gps, measures }
        storage.append_route_point(point)
        storage.end_route()

    storage.backup_cache()

    // --- 7. Display (synchronous, no worker task) ---
    DisplayService display(display_config)
    DisplayValues values = build_fast_path_display(measures, gps, bms_snap, settings)
    display.update_sync(values)

    // --- 8. BMS ---
    BQ25XX bms(i2c_bus, I2C_ADDR_BMS)
    bms.init()
    PowerService power(bms, gpio::native::hal, power_config)
    power.reset_watchdog()

    // --- 9. Save state and re-enter deep sleep ---
    power.save_state(state)
    uint32_t next_ms = compute_fast_path_sleep_duration(settings)
    power.enter_sleep(PowerService::SleepType::Deep, next_ms)
    // never returns
```

### build_fast_path_display()

Free function that builds a minimal `DisplayValues` for the locked
dashboard without a UIManager:

- Sensor readings from the one-shot measurement
- Battery status from BMS poll
- GPS status (fix valid, satellite count)
- `locked = true`, `screen = Screen::Home`
- Settings-derived flags (`use_fahrenheit`, `pm_use_usaqi`)
- No chart data, no menu state, no snackbar

### compute_fast_path_sleep_duration(settings)

Same logic as `Orchestrator::compute_sleep_duration_ms()`:

```
duration = settings.measurement_interval_seconds * 1000
if settings.display_refresh_interval_seconds > 0:
    display_ms = settings.display_refresh_interval_seconds * 1000
    duration = min(duration, display_ms)
return duration
```

This is a free function because it runs outside the Orchestrator class.

## Helper Functions

### init_nvs()

```cpp
static void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}
```

### init_i2c_bus()

```cpp
static i2c_master_bus_handle_t init_i2c_bus() {
    i2c_master_bus_config_t config = {
        .i2c_port              = I2C_NUM_0,
        .sda_io_num            = PIN_I2C_SDA,
        .scl_io_num            = PIN_I2C_SCL,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt     = 7,
        .intr_priority         = 0,
        .trans_queue_depth     = 0,
        .flags = { .enable_internal_pullup = true },
    };
    i2c_master_bus_handle_t bus = nullptr;
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
    return bus;
}
```

### init_gpio()

Configures power-enable pins and sets initial levels. Exact pins depend on
the AGo schematic. Follows the reference product pattern:

```
init_gpio():
    // Use gpio::native::hal for all GPIO operations
    // Configure output pins for sensor/peripheral power enables
    // Set initial levels (high = enable or low = disable per schematic)
    // Set drive strength as needed
```

The reference product configures individual output pins, sets max drive
capability, and sets initial levels. AGo will follow the same pattern with
its own pin set from `board_config.h`.

### init_spi_buses()

Configures one or two SPI hosts depending on whether display and NAND share
a bus:

```
init_spi_buses():
    // Display SPI bus
    spi_bus_config_t display_bus = {
        .mosi_io_num = PIN_DISPLAY_MOSI,
        .sclk_io_num = PIN_DISPLAY_CLK,
        ...
    }
    spi_bus_initialize(SPI_HOST_DISPLAY, &display_bus, SPI_DMA_CH_AUTO)

    // NAND SPI bus (if separate host)
    spi_bus_config_t nand_bus = {
        .mosi_io_num = PIN_NAND_MOSI,
        .miso_io_num = PIN_NAND_MISO,
        .sclk_io_num = PIN_NAND_CLK,
        ...
    }
    spi_bus_initialize(SPI_HOST_NAND, &nand_bus, SPI_DMA_CH_AUTO)
```

Whether display and NAND share an SPI host or use separate hosts depends on
hardware routing. If they share, chip-select arbitration is handled by the
SPI driver.

## Error Handling

All bus initialization steps use `ESP_ERROR_CHECK` — a bus failure is fatal
because multiple subsystems depend on it.

For individual sensor driver `init()` failures: log and continue.
`SensorManager` handles `nullptr` sensors gracefully. A failed sensor does
not block the rest of the system.

```
if (!sht40.init()) {
    ESP_LOGE(TAG, "SHT40 init failed");
    sensors.temp_hum = nullptr;     // SensorManager will skip
}
```

For NAND mount failure: `StorageService::init()` returns false.
Temporary cache still works (RTC-backed). Route persistence is unavailable
until the issue is resolved (e.g., filesystem corruption, hardware fault).

## Design Decisions

### No BSP Layer

AGo has a single board revision. A BSP abstraction (pin tables, board
detection, variant selection) adds a layer between `main.cpp` and the
hardware without providing any value. Pin assignments live in
`board_config.h` and initialization is inline in `main.cpp`. If a second
board revision appears, a BSP layer can be introduced at that time.

### All Initialization Inline in main.cpp

Keeps the entire construction order visible in one function. No hidden
dependencies between initialization helpers. The cost is a long function,
but the logic is strictly linear — no branching except the boot path
selection at the top.

### Stack-Allocated Objects

All driver instances and services are local variables in `app_main()`. Since
the function never returns (the orchestrator loop is infinite), they live
for the duration of the program. No heap allocation needed for service
instances. References in the `Orchestrator::Services` struct point to these
stack objects.

### Settling Delays

100 ms delays after GPIO power-enable and I2C bus creation match the
reference product pattern and allow capacitors to charge and I2C pull-ups to
stabilize. These can be tuned during hardware bring-up.

### Fast-Path NVS Load

The fast-path needs `GoSettings` for sleep duration and GPS mode. This
requires `init_nvs()` + `ConfigStore::init()` + `load_go_settings()`. The
NVS init cost is negligible (~5 ms) compared to the measurement cycle
(~2 s). Alternatives (storing settings in RTC memory) were considered but
rejected because they expand the RTC struct and introduce a second source of
truth for configuration.

### Sensor Serial Interface

The CO2 sensor and PM sensor may use native UART (`AirgradientUART`) or an
I2C-to-UART bridge (`AirgradientIICSerial`) depending on hardware routing.
Both implement `AirgradientSerial`, so the sensor driver constructors are
agnostic. The concrete serial type is selected at construction time in
`main.cpp` based on the board design.
