# AirGradient Firmware

This repository contains the AirGradient ESP-IDF firmware monorepo for
AirGradient product models.

The project is organized around shared reusable components and thin
product-specific application roots. The goal is to keep firmware logic
testable, readable, and reusable across multiple AirGradient devices.

## Current Status

Today it includes:

- shared firmware components under `components/`
- the **AirGradient Go** portable monitor under `products/go/`
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

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"

# AirGradient Go
idf.py -C products/go build

# Reference product
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
- `products/go/build/compile_commands.json` for the AGo ESP-IDF build
- `products/reference/build/compile_commands.json` for the reference ESP-IDF build

To create a single root-level `compile_commands.json` for editor discovery,
build whichever targets you need, then merge them:

```sh
python scripts/merge_compile_commands.py
```

The merge prefers host-test entries for shared files that appear in both
databases, then includes the remaining firmware-only entries.

## Documentation

Start at the layer that matches your task:

- [`components/README.md`](components/README.md) — shared component layout;
  each component carries its own `README.md`
- [`products/README.md`](products/README.md) — product application roots;
  each product carries its own `README.md`, `ARCHITECTURE.md`, `docs/`,
  and `specs/`
- [`tests/README.md`](tests/README.md) — host-test workflow

When adding or editing any Markdown file:

- [`docs/STYLE.md`](docs/STYLE.md) — documentation style guide
- [`docs/templates/`](docs/templates) — copy-pasteable templates per doc type
