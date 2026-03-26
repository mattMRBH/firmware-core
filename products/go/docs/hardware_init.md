# Hardware Initialization

Boot path selection, hardware initialization, and fast-path boot for the
AirGradient Go product. All initialization logic lives in `main.cpp` with pin
assignments in `board_config.h`.

## Files

| File | Purpose |
|---|---|
| `main/main.cpp` | `app_main()`, boot path selection, hardware init, fast-path boot |
| `main/board_config.h` | Pin assignments and peripheral constants (TBD placeholders) |
| `main/go_orchestrator.h` | Orchestrator class declaration |
| `main/go_orchestrator.cpp` | Full orchestrator implementation: dispatch, timers, state transitions, sleep |

## Boot Path Selection

`app_main()` determines the boot path before any peripheral initialization
using two static helpers from `PowerService`:

```
app_main():
    cause = PowerService::get_wake_cause()

    if cause == Timer:
        state = load_rtc_app_state()     // free function, no deps
        if PowerService::is_fast_path_wake(cause, state):
            run_fast_path(state)         // never returns

    run_full_boot(cause)                 // never returns
```

| Wake Cause | Lock State | Path |
|---|---|---|
| `PowerOn` | -- | Full boot with default state |
| `Timer` + `Locked` | Locked | Fast-path: measure, display, sleep |
| `Timer` + `Unlocked` | Unlocked | Full boot (should not normally happen) |
| `Button` | -- | Full boot, restore state from RTC, unlock |

### load_rtc_app_state()

Free function declared in `go_power.h`, implemented in `go_power.cpp`. Reads
the same `RTC_DATA_ATTR` static variables that `PowerService::save_state()` /
`load_state()` use. Safe to call before `PowerService` is constructed because
it has no dependencies on I2C, BMS, or any other peripheral.

## Full Initialization Sequence

`run_full_boot(cause)` initializes all hardware and services in strict
dependency order, then hands control to the Orchestrator:

| # | What | Why this order |
|---|---|---|
| 1 | NVS | Settings depend on NVS |
| 2 | Settings (`ConfigStore` + `load_go_settings`) | Configuration needed for service construction |
| 3 | GPIO (power enables) + 100 ms settling | Power enables must be set before drivers access peripherals |
| 4 | I2C bus + 100 ms settling | Sensors, BMS, touch all share the I2C bus |
| 5 | SPI buses (display + NAND) | Display and NAND on SPI |
| 6 | Sensor drivers (SHT40, SGP41, S8, PMS5003) | Must exist before SensorManager |
| 7 | SensorManager | Wraps sensor drivers for averaging |
| 8 | BMS (BQ25XX) | Independent of sensors; needed for PowerService |
| 9 | GPS (NmeaGps via AirgradientUART) | UART is constructed here; `begin()` is called by `GpsService::run()` when the task starts |
| 10 | Touch (CAP1203) | I2C; needed for InputService |
| 11 | Storage (PayloadCache + SpiNandStorage + StorageService) | NAND mount + cache restore before orchestrator runs |
| 12 | Event queue | All producer services need the queue handle |
| 13 | Services (SensorProducer, GpsService, InputService, DisplayService, PowerService, UIManager, BLE) | Depend on all drivers and infrastructure above; PowerService also initializes and first-pulses the external watchdog |
| 14 | Display init | Show initial screen before event loop |
| 15 | Start producer tasks | Services ready to produce events |
| 16 | Orchestrator | Last — owns the event loop; `run()` never returns |

All objects are stack-allocated in `run_full_boot()`. Since `Orchestrator::run()`
never returns, they live for the duration of the program.

## Fast-Path Boot

Timer wake while locked. Minimal initialization — no event loop, no producer
tasks, no input handling. Goal: measure, display, sleep.

| # | What |
|---|---|
| 0 | NVS + settings (for sleep duration, GPS mode) |
| 1 | GPIO + 100 ms settling |
| 2 | I2C bus + 100 ms settling |
| 3 | SPI buses |
| 4 | Sensor drivers + init (same construction as full boot) |
| 5 | One-shot measurement (`SensorManager::start_measures(1)`) |
| 6 | One-shot GPS if tracking + GPS active (`gps_read_once()`) |
| 7 | Storage: restore cache, init NAND, cache measurement, route point if tracking |
| 8 | BMS: init, poll for display data, reset watchdog |
| 8a | External watchdog: init + first pulse (via PowerService) |
| 9 | Display: synchronous update (`update_sync()`, no worker task) |
| 10 | Save state + re-enter deep sleep |

## Error Handling

- **Bus init failures** (I2C, SPI, NVS): `ESP_ERROR_CHECK` — fatal, device
  cannot function without buses.
- **Sensor init failures**: Log and set `Sensors` pointer to `nullptr`.
  `SensorManager` handles `nullptr` sensors gracefully. A failed sensor does
  not block the rest of the system.
- **NAND mount failure**: `StorageService::init()` returns false. Temporary
  cache still works (RTC-backed). Route persistence is unavailable.
- **BMS init failure**: Logged. PowerSnapshot returns invalid sentinels.

## Board Configuration

All pin assignments in `board_config.h` are placeholder values (`-1`) pending
hardware schematic finalization. Known I2C addresses use datasheet defaults:

| Device | Address |
|---|---|
| SHT40 | 0x44 |
| SGP41 | 0x59 |
| BQ25XX | 0x6B |
| CAP1203 | 0x28 |

Peripheral bus assignments:

| Peripheral | Bus |
|---|---|
| Display (SSD1680) | SPI2_HOST |
| NAND flash | SPI3_HOST |
| GPS (NmeaGps) | UART_NUM_1 |
| CO2 (S8/Sunlight) | UART_NUM_0 |
| PM (PMS5003) | UART_NUM_2 |

## Serial Number and BLE Name

`app_main()` builds a 12-character device serial number via
`build_serial_number()` from `airgradient-common` before full boot. In
Portable mode, the BLE service advertises as `AGo-<serial>`.

## Design Decisions

See [specs/hardware_init.md](../specs/hardware_init.md) for detailed rationale
on: no BSP layer, inline initialization in main.cpp, stack-allocated objects,
settling delays, fast-path NVS load, and sensor serial interface selection.
