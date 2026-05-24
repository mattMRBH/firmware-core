# AirGradient Go

Firmware for the [AirGradient Go](https://www.airgradient.com/portable/)
portable air quality monitor with GPS, e-paper display, BLE, and battery.

## Sensors

- **PM** — Sensirion SPS30 (PM1.0, PM2.5, PM10)
- **CO2** — SenseAir S12 / Sensirion SCD4x / Sensirion STCC4 (probed in
  order at boot; first detected wins)
- **TVOC / NOx** — Sensirion SGP41
- **Temp / Humidity** — fallback from CO2 and pressure sensors
- **Pressure** — Infineon DPS368
- **Battery** — TI BQ25628 / 29 charger IC

## Hardware Notes

The SPS30 PM sensor is powered by the PMID 5 V rail, which is supplied by
the BQ25628 / 29 OTG boost converter. BMS initialization must complete
before sensor drivers are initialized — otherwise the SPS30 will not
respond on I2C.

For AGo hardware, firmware configures PMID explicitly:

- **USB / external power present** → PMID **pass-through**
- **No external power** → PMID **5 V boost**

The runtime power path is also re-evaluated while the device is running so
a plug / unplug event can switch the SPS30 supply without rebooting.

## Build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/go build
```

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — boot paths, event model, module
  structure
- [`docs/`](docs) — per-service implementation notes (BLE, display, GPS,
  input, orchestrator, power, sensor producer, settings, storage, UI,
  Wi-Fi)
- [`specs/`](specs) — design specs and refactor plans (temporary; deleted
  once shipped, per [`docs/STYLE.md`](../../docs/STYLE.md))
- [`tests/`](tests) — host tests (`go_*.tests.cpp`) plus the BLE
  integration suite under `tests/ble-integration/`
- `main/board_config.h` — pin assignments and I2C addresses
