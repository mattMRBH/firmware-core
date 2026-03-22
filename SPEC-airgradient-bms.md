# Spec: `airgradient-bms` Component

## Overview

This spec defines a new shared `airgradient-bms` component and removes battery
management concerns from `airgradient-sensors`.

The goal is full separation:

- `airgradient-sensors` owns environmental sensing and shared measurement
  orchestration via `SensorManager`
- `airgradient-bms` owns battery-management hardware drivers and BMS-specific
  telemetry types
- product or service code owns power policy, watchdog cadence, sleep decisions,
  and shutdown behavior

Initial target hardware is the existing BQ25XX family support (`BQ25672`,
`BQ25798`) currently implemented inside `components/airgradient-sensors/`.

## Background Problem

The current repository places the `BQ25XX` battery-management driver under
`components/airgradient-sensors/` and exposes it through a
`BatteryMgmtSensor` HAL. This creates an architectural mismatch:

- the BQ25XX is not a passive environmental sensor
- the device has control responsibilities such as watchdog reset and future
  ship-mode / QoN entry
- `SensorManager` is intended to average measurement data from sensors, but BMS
  ownership belongs with power-related code
- `Measures` currently includes `power`, which couples shared sensor results to
  a product-specific power-management concern

This has already leaked into product design. AirGradient Go treats the BMS as
owned by `PowerService`, while `SensorManager` receives `battery_mgmt = nullptr`
and leaves `Measures.power` invalid.

That split is a strong signal that the shared architecture should separate BMS
from sensors instead of keeping a partially-unused battery path inside the
sensor stack.

## Goals

- create a dedicated shared `airgradient-bms` component
- move the existing `BQ25XX` driver out of `airgradient-sensors`
- remove BMS-specific HALs, types, and orchestration from
  `airgradient-sensors`
- remove battery fields from the shared `Measures` aggregate so
  `SensorManager` only represents sensor measurements
- keep the new BMS component reusable across products
- preserve host-test friendliness under `TEST_HOST`

## Non-Goals

- defining product-specific power services such as `PowerService`
- changing sleep policy, RTC persistence, or shutdown UX
- implementing QoN / ship-mode support in this spec
- defining a shared cross-product power-management service layer

Those topics can build on top of this component split in follow-up work.

## Architecture

### Target Layering

```text
product power code -> BmsDevice (hal/) -> BQ25XX (drivers/) -> I2C + RTOS
product sensor code -> SensorManager -> environmental sensors only
```

After this change:

- `airgradient-sensors` contains Temp/Humidity, PM, CO2, TVOC/NOx, and O3/NO2
  interfaces, drivers, and orchestration
- `airgradient-bms` contains battery-management interfaces, public BMS types,
  and concrete charger / PMIC drivers
- products compose both components as needed, but the shared sensor manager no
  longer knows anything about BMS devices

## Directory Structure

```text
components/airgradient-bms/
  hal/
    bms_device.h                 # Abstract BMS interface
  types/
    bms_types.h                  # Public BMS telemetry types and validation
  drivers/
    bq25xx/
      bq25xx.h                   # BQ25672/BQ25798 driver header
      bq25xx.cpp                 # Driver implementation
  tests/
    CMakeLists.txt               # Component-local native test build
    bms_types.tests.cpp          # Sentinel and validation tests
    bq25xx.tests.cpp             # Driver unit tests / scaffolding
  CMakeLists.txt                 # ESP-IDF component registration
  README.md                      # Component documentation
```

This mirrors the role-based layout already used by other shared components.

## Public API

### HAL Interface: `hal/bms_device.h`

The existing `BatteryMgmtSensor` name should not move forward into the new
component. The abstraction is no longer "a sensor". Replace it with a BMS-
focused interface.

```cpp
class BmsDevice {
public:
    virtual ~BmsDevice() = default;

    virtual bool init() = 0;

    // Telemetry
    virtual bool read_telemetry(BmsTelemetry& out) = 0;
    virtual bool read_status(BmsStatus& out) = 0;

    // Optional hardware features
    virtual bool feature_ship_available() const = 0;
    virtual bool enter_ship_mode() = 0;
};
```

Design notes:

- avoid a single generic `read()` method; public API should state what group of
  data is being requested
- capability checks belong in the HAL when hardware support is optional
- policy still lives above the HAL; the HAL only exposes hardware primitives
  such as ship-mode availability and entry
- extended chip-specific methods can still remain on concrete drivers when they
  are not yet good candidates for the generic interface

## Public Types

### `types/bms_types.h`

Move battery-management data out of `airgradient-common/include/measures_types.h`
into a BMS-owned public header.

Suggested initial type:

```cpp
namespace BmsInvalid {
static constexpr float VOLT = -1.0f;
}

enum class BmsChargingState : uint8_t {
    Unknown,
    NotCharging,
    TrickleCharge,
    PreCharge,
    FastCharge,
    TaperCharge,
    TopOffTimerActiveCharging,
    ChargeTerminationDone,
};

struct BmsTelemetry {
    float battery_voltage;
    float charging_voltage;

    bool is_battery_voltage_valid() const;
    bool is_charging_voltage_valid() const;
    bool is_valid() const;
};

struct BmsStatus {
    BmsChargingState charging_state = BmsChargingState::Unknown;

    bool is_charging_state_valid() const;
    bool is_valid() const;
};
```

Notes:

- `BmsTelemetry` replaces `BatteryMgmtData`
- `BmsChargingState` replaces the driver-local `BQ25XX::ChargingStatus` enum as
  the shared public status type
- validation remains field-level and sentinel-based
- invalid constants should live with the BMS type, not in sensor measurement
  aggregates
- separate types make future expansion easier; voltage telemetry and charger
  state do not need to evolve in lockstep
- if later products need current, temperature, faults, or input-power status,
  those can be added as additional BMS-owned types and read methods without
  re-coupling to `SensorManager`

## Driver: `drivers/bq25xx/`

Move the existing driver implementation unchanged in behavior, but update its
ownership and naming:

- old location:
  `components/airgradient-sensors/drivers/bq25xx/`
- new location:
  `components/airgradient-bms/drivers/bq25xx/`

Expected header shape:

```cpp
class BQ25XX : public BmsDevice {
public:
    explicit BQ25XX(i2c_master_bus_handle_t i2c_bus,
                    uint8_t address = ADDRESS_DEFAULT);

    bool init() override;
    bool read_telemetry(BmsTelemetry& out) override;
    bool read_status(BmsStatus& out) override;

    bool feature_ship_available() const override;
    bool enter_ship_mode() override;

    bool update_watchdog();
    bool get_battery_percentage(float* output);
    bool get_battery_current(int16_t* output);
    bool get_temperature(float* output);
    BmsChargingState get_charging_status();
};
```

Naming cleanup is encouraged when touching this API:

- `updateWatchdog()` -> `update_watchdog()`
- `getBatteryPercentage()` -> `get_battery_percentage()`
- `getChargingStatus()` -> `get_charging_status()`
- `read()` -> `read_telemetry()`

However, that cleanup is optional if it would make the migration noisier than
necessary. The architectural split is the primary goal.

## `airgradient-bms` CMake

Initial component registration:

```cmake
idf_component_register(
    SRCS "drivers/bq25xx/bq25xx.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common esp_driver_i2c
)
```

Notes:

- `airgradient-common` remains a dependency for RTOS abstractions
- `esp_driver_i2c` remains required by the concrete driver
- there is no dependency on `airgradient-sensors`

## Removal From `airgradient-sensors`

This spec requires complete removal of BMS concerns from the shared sensor
component.

### Files Removed From `airgradient-sensors`

- `hal/battery_mgmt_sensor.h`
- `drivers/bq25xx/bq25xx.h`
- `drivers/bq25xx/bq25xx.cpp`

### `SensorManager` Changes

Remove BMS from the shared sensor wiring and averaging logic.

#### `services/sensor_manager.h`

- remove `#include "hal/battery_mgmt_sensor.h"`
- remove `BatteryMgmtSensor* battery_mgmt;` from `struct Sensors`
- remove battery counters from `AverageMeasuresCounters`
- remove `_accumulate_battery(...)`
- remove `_calculate_battery_average(...)`

#### `services/sensor_manager.cpp`

- remove battery sum initialization
- remove battery accumulation calls from `start_measures()`
- remove battery average assignment to the result
- delete the battery accumulation and averaging helper implementations

After this change, `SensorManager` only handles environmental measurements.

### `measures_types.h` Changes

Shared sensor measures should no longer carry battery data.

#### Remove

```cpp
struct BatteryMgmtData {
    float volt_battery;
    float volt_charging;

    bool is_vbat_valid() const;
    bool is_vpanel_valid() const;
    bool is_valid() const;
};
```

and remove this field from `Measures`:

```cpp
BatteryMgmtData power;
```

This is the key "full separation" point. Battery telemetry becomes a BMS/power
concern, not part of the sensor aggregate.

### Sensor Tests

Update `components/airgradient-sensors/tests/sensor_manager.tests.cpp` to
remove:

- `MockBatteryMgmtSensor`
- battery-related setup in the default `Sensors` fixture
- battery averaging assertions
- battery failure / null-path cases

The resulting sensor-manager tests should verify only the remaining supported
sensor families.

### Sensor Documentation

Update `components/airgradient-sensors/README.md`:

- remove `hal/battery_mgmt_sensor.h` from the HAL list
- remove `drivers/bq25xx/` from the drivers list
- update prose so the component is clearly environmental-sensor focused

## Repository-Level Documentation Updates

Update shared docs to reflect the new ownership:

- `components/README.md`
  - add `airgradient-bms/`
  - update `airgradient-sensors/` description to exclude BMS ownership
- `README.md`
  - update any shared-component listings if they mention sensor ownership in a
    way that still implies BMS inclusion

## Implementation Sequence

Recommended order:

1. Create `components/airgradient-bms/` with `hal/`, `types/`, `drivers/`,
   `tests/`, `README.md`, and `CMakeLists.txt`
2. Move `BQ25XX` into the new component
3. Introduce `BmsDevice` and `BmsTelemetry`
4. Remove `BatteryMgmtSensor` and `BatteryMgmtData`
5. Remove BMS members and battery aggregation from `SensorManager`
6. Remove battery fields from `Measures`
7. Update sensor tests to match the narrower scope
8. Update component READMEs and repository docs

This order keeps ownership changes clear and makes the API breakage deliberate
instead of gradual.

## Validation Requirements

The implementation that follows this spec should verify:

- `airgradient-bms` builds independently as a shared component
- `airgradient-sensors` no longer includes or references BMS headers or source
  files
- `SensorManager` compiles without any battery-related code paths
- `Measures` no longer contains BMS data
- BMS public types still use invalid sentinels and field-level validation
- host tests for `airgradient-sensors` no longer depend on a battery mock
- new or moved BMS tests compile under `TEST_HOST`

## Follow-Up Work

This spec intentionally stops at the shared-component boundary. Separate follow-
up work will be needed to:

- update product includes and component dependencies to use `airgradient-bms`
- update product code that currently includes
  `drivers/bq25xx/bq25xx.h` from `airgradient-sensors`
- replace `BatteryMgmtData` usage with `BmsTelemetry`
- replace `BQ25XX::ChargingStatus` usage with `BmsChargingState`
- update product specs and docs that currently describe BMS as living under
  `airgradient-sensors`

Those changes should be handled in a product integration spec after this
component split is accepted.
