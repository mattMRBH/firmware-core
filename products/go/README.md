# AirGradient Go

Firmware for the [AirGradient Go](https://www.airgradient.com/portable/)
portable air quality monitor.

## Sensors

- **PM** — Sensirion SPS30 (PM1.0, PM2.5, PM10)
- **CO2** — Senseair STCC4
- **TVOC / NOx** — Sensirion SGP41
- **Temp / Humidity** — fallback from CO2 and pressure sensors
- **Pressure** — Infineon DPS368
- **Battery** — TI BQ25628/29 charger IC

## Hardware Notes

The SPS30 PM sensor is powered by the PMID 5V rail, which is supplied by
the BQ25628/29 OTG boost converter. BMS initialization must complete before
sensor drivers are initialized — otherwise the SPS30 will not respond on I2C.

## Build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/go build
```

## Documentation

- `ARCHITECTURE.md` — boot paths, event model, and module structure
- `docs/` — feature specifications and design notes
- `main/board_config.h` — pin assignments and I2C addresses
