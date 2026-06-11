# Reference Product

Thin ESP-IDF product entrypoint used to validate the shared component
layout and as a hardware bring-up sandbox for new components.

## Status

`Scaffold` — not a shipping product. `main/` contains `test_*.cpp` smoke
tests for BLE, GPIO, NAND storage, OTA, payload cache, sensors, serial, and
touch components, used to exercise components on real hardware while the
multi-product structure takes shape.

## Sensors

This product does not have a fixed sensor configuration. Bring-up tests
under `main/test_sensors.{cpp,h}` exercise concrete drivers from
`components/airgradient-sensors/` against whatever hardware is wired up.

## Build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference build
```

## Documentation

- `main/board_config.h` — current bring-up pin assignments and I2C
  addresses
- `main/test_*.cpp` — per-component bring-up smoke tests

## Notes

When this product graduates from scaffold to a shipping product, replace
the bring-up smoke tests with real product wiring and add an
`ARCHITECTURE.md` plus `docs/` and `specs/` directories per the
[product README template](../../docs/templates/product_readme.md).
