# Airgradient-BMS Component

This component owns the shared battery management system (BMS) stack.

It provides:

- a HAL interface for BMS devices
- BMS-specific public types (telemetry, status, charging state)
- concrete charger / PMIC drivers

BMS concerns are separated from the environmental sensor stack
(`airgradient-sensors`). Product-level power policy, watchdog cadence, sleep
decisions, and shutdown behavior live outside this component.

## Directory layout

```text
components/airgradient-bms/
  hal/
  types/
  drivers/
  tests/
  CMakeLists.txt
```

- `hal/` - abstract BMS device interface (`BmsDevice`)
- `types/` - public BMS types: `BmsTelemetry`, `BmsStatus`, `BmsChargingState`, `BmsPowerSource`, inline `_str` helpers
- `drivers/` - concrete BMS driver implementations grouped by IC family
- `tests/` - host-side tests owned by this component
- `CMakeLists.txt` - ESP-IDF component registration

## Public include layout

Use includes by role:

```cpp
#include "hal/bms_device.h"
#include "types/bms_types.h"
#include "drivers/bq25xx/bq25xx.h"
#include "drivers/bq25629/bq25629_bms.h"
```

Guideline:

- include from `hal/` when depending on the abstract interface
- include from `types/` when depending on BMS data types only
- include from `drivers/` only when instantiating a concrete implementation

## Current contents

### HAL

- `hal/bms_device.h` - abstract `BmsDevice` interface

### Types

- `types/bms_types.h` — public BMS types, sentinels, and validation helpers:
  - `BmsChargingState` — charging state enum
  - `BmsPowerSource` — power source / adapter type enum
  - `BmsTelemetry` — full ADC snapshot (voltages, currents, temperatures)
  - `BmsStatus` — charger status (charging state, power source, regulation
    and fault flags)
  - `BmsInvalid` — sentinel values for all field types
  - `bms_charging_state_str()` / `bms_power_source_str()` — inline
    human-readable enum-to-string helpers

### Drivers

- `drivers/bq25xx/` - BQ25672/BQ25798 battery charger IC driver
- `drivers/bq25629/` - BQ25629 single-cell charger IC adapter (wraps vendor `bq25629` component)

### Tests

- `tests/bms_types.tests.cpp` - sentinel and validation tests
- `tests/CMakeLists.txt` - component-local native test build

## Dependencies

This component depends on:

- `components/airgradient-common/` for RTOS abstraction
- `components/bq25629/` for the BQ25629 vendor driver
- `esp_driver_i2c` for I2C master bus access (concrete driver)

There is no dependency on `airgradient-sensors`.

## Host tests

Host tests live in `components/airgradient-bms/tests/` and are executed through
the root `tests/` runner.

See:

- `tests/README.md` for host-test build and run commands
- `components/airgradient-bms/tests/CMakeLists.txt` for the component-local
  test target wiring
