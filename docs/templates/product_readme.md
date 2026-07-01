# AirGradient \<Product\>

Firmware for the AirGradient \<Product\> (one-line description and a link to
the product page if applicable).

## Sensors

- **PM** — vendor model (PM1.0, PM2.5, PM10)
- **CO2** — vendor model
- **TVOC / NOx** — vendor model
- **Temp / Humidity** — vendor model or fallback source
- **Pressure** — vendor model
- **Battery** — vendor PMIC

## Hardware Notes

Capture product-specific quirks that affect bring-up order, power rails, or
runtime behavior. For example: which rail powers which sensor, required
init ordering, runtime power-path re-evaluation.

## Build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/<product> build
```

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — boot paths, event model, module
  structure
- [`docs/`](docs) — feature specifications and design notes
- [`specs/`](specs) — design specs and refactor plans
- `main/board_config.h` — pin assignments and I2C addresses
