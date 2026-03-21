# AirGradient Firmware

This repository contains the AirGradient ESP-IDF firmware monorepo for
AirGradient product models.

The project is organized around shared reusable components and thin
product-specific application roots. The goal is to keep firmware logic
testable, readable, and reusable across multiple AirGradient devices.

## Current Status

This repository is currently in the foundation stage.

Today it includes:

- shared firmware components under `components/`
- a thin reference ESP-IDF product under `products/reference/`
- host-side test entrypoints under `tests/`

## Repository Layout

- `components/` - shared AirGradient ESP-IDF components, including shared
  drivers, HALs, services, and third-party code when it belongs to a shared
  component responsibility
- `products/` - AirGradient product-specific ESP-IDF application roots
- `tests/` - top-level host-test entrypoint

## Key Ideas

- shared capability code lives in `components/`
- product-specific wiring and BSP live in `products/`
- application logic should stay host-testable where practical
- product application roots should stay thin

## Build Firmware

Example build for the reference product:

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference build
```

## Run Host Tests

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

## Editor Compile Database

For clangd or Neovim LSP, this repo can generate compile databases in multiple
build roots:

- `tests/build/compile_commands.json` for native host tests
- `products/reference/build/compile_commands.json` for the ESP-IDF reference build

To create a single root-level `compile_commands.json` for editor discovery,
build whichever targets you need, then merge them:

```sh
python scripts/merge_compile_commands.py
```

The merge prefers host-test entries for shared files that appear in both
databases, then includes the remaining firmware-only entries.

## More Documentation

- `components/README.md`
- `products/README.md`
- `tests/README.md`
