# Host Tests

This repository uses `tests/` as the top-level host-test entrypoint.

For the `airgradient-sensors` component structure itself, see `components/airgradient-sensors/README.md`.

## What lives here

- `tests/CMakeLists.txt` is the top-level CMake entrypoint for host tests
- `tests/Makefile` provides convenience wrappers for configure, build, run, and clean
- `components/airgradient-sensors/tests/` contains the current sensors-owned host test suite

Right now the active suite focuses on `SensorManager` averaging, fallback, null-handling, and iteration timing behavior.

## How the test runner is wired

- `tests/CMakeLists.txt` is the top-level host-test entrypoint
- `tests/CMakeLists.txt` fetches Catch2 and Trompeloeil, then delegates to `components/airgradient-sensors/tests/`
- `components/airgradient-sensors/tests/CMakeLists.txt` builds:
  - `sensors_test_support` - host-side support library for `SensorManager`
  - `sensors_tests` - Catch2 executable for the sensors component

## Build host tests

From the repository root:

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
```

Or use the wrapper from `tests/`:

```sh
cd tests
make build
```

## Build only the component tests

From the repository root:

```sh
cmake -S components/airgradient-{{component}}/tests -B components/airgradient-sensors/tests/build
cmake --build components/airgradient-{{component}}/tests/build
```

This standalone mode is useful when iterating only on specific components (only if has tests)

## Run all host tests

From the repository root:

```sh
ctest --test-dir tests/build --output-on-failure
```

Or from `tests/`:

```sh
cd tests
make test
```

## Run with verbose output

From `tests/`:

```sh
make run
```

That currently expands to:

```sh
ctest --test-dir build --output-on-failure -V
```

## Run only the sensors test executable

After building, run the component-local Catch2 binary directly:

```sh
./tests/build/components_airgradient_sensors_tests/sensors_tests
```

Useful Catch2 commands:

```sh
./tests/build/components_airgradient_sensors_tests/sensors_tests --list-tests
./tests/build/components_airgradient_sensors_tests/sensors_tests --list-tags
./tests/build/components_airgradient_sensors_tests/sensors_tests "Averaging"
./tests/build/components_airgradient_sensors_tests/sensors_tests "[SensorManager]"
./tests/build/components_airgradient_sensors_tests/sensors_tests -s
```

## Clean host-test build artifacts

From `tests/`:

```sh
make clean
```

Or from the repository root:

```sh
rm -rf tests/build
```

## Adding future component tests

- Add new `*.tests.cpp` files under their owning component test directory
- Register those files in `components/airgradient-sensors/tests/CMakeLists.txt`
- Keep the root `tests/` directory as the single shared host-test entrypoint
- If a new test still targets `SensorManager`, it can stay here even if it covers a different behavior area

## Current test inventory

Current Catch2 test case list:

- `Averaging` with tag `[SensorManager]`

Current CTest registration:

- `Averaging`
