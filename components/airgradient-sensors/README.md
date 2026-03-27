# Airgradient-Sensors Component

This component owns the shared environmental sensor stack for the firmware.

It keeps the existing architecture intact:

- HAL interfaces for sensor types
- concrete sensor drivers under this component
- `SensorManager` as the shared sensor orchestrator

Battery management (BMS) concerns live in the separate `airgradient-bms`
component.  This component handles only environmental measurements.

## Directory layout

```text
components/airgradient-sensors/
  hal/
  drivers/
  services/
  tests/
  CMakeLists.txt
```

- `hal/` - public sensor interfaces such as `TempHumSensor`, `PMSensor`, and `CO2Sensor`
- `drivers/` - concrete sensor implementations grouped by driver family
- `services/` - shared sensor orchestration logic, currently `SensorManager`
- `tests/` - host-side tests owned by this component
- `CMakeLists.txt` - ESP-IDF component registration for the unified `airgradient-sensors` component

## Public include layout

Use includes by role:

```cpp
#include "hal/temp_hum_sensor.h"
#include "services/sensor_manager.h"
#include "drivers/sht40/sht40.h"
```

Guideline:

- include from `hal/` when depending on an interface
- include from `services/` when using shared orchestration logic
- include from `drivers/` only when instantiating a concrete implementation

## Current contents

### HAL

- `hal/temp_hum_sensor.h`
- `hal/pm_sensor.h` — includes optional `supports_temp_hum()` / `temp_hum_data()` for PM sensors with integrated temperature and humidity (e.g. PMS5003T)
- `hal/co2_sensor.h` — includes optional `supports_temp_hum()` / `temp_hum_data()` for CO2 sensors with integrated temperature and humidity (e.g. STCC4)
- `hal/tvoc_nox_sensor.h`
- `hal/o3_no2_sensor.h`

### Drivers

- `drivers/sht40/` — Sensirion SHT40 temperature and humidity sensor (I2C)
- `drivers/pms5003/` — Plantower PMS5003 and PMS5003T particulate matter sensors (serial via IIC bridge)
- `drivers/sps30/` — Sensirion SPS30 particulate matter sensor (I2C). Maps mass concentrations to atmospheric PM fields and number concentrations to particle count fields. Fields not provided by SPS30 (standard particle, pm_5_pc) are left as invalid sentinels.
- `drivers/s8/` — SenseAir S8 CO2 sensor (Modbus RTU over serial)
- `drivers/sunlight/` — SenseAir Sunlight CO2 sensor (Modbus RTU over serial)
- `drivers/stcc4/` — Sensirion STCC4 CO2 sensor with integrated temperature and humidity (I2C). Implements `CO2Sensor` with `supports_temp_hum() = true`.
- `drivers/sgp41/` — Sensirion SGP41 TVOC and NOx sensor (I2C)
- `drivers/alpha_sense/` — AlphaSense O3/NO2 electrochemical sensor (via dual ADS1115)
- `drivers/co2_common/` — shared Modbus CRC helper used by S8 and Sunlight CO2 drivers

### Services

- `services/sensor_manager.h` — `SensorGroup` enum, `SensorManager` class
- `services/sensor_manager.cpp`

#### SensorGroup

`SensorGroup` is a bitmask enum that controls which sensors `start_measures()` polls:

| Value   | Sensors polled |
|---------|----------------|
| `PM`    | `pms_a`, `pms_b` |
| `Other` | `temp_hum`, `co2`, `tvoc_nox`, `o3_no2`, `pressure` |
| `All`   | All of the above (default) |

Combine with `operator|` and test with `has_group()`:

```cpp
SensorGroup groups = SensorGroup::PM | SensorGroup::Other; // == All
bool pm = has_group(groups, SensorGroup::PM); // true
```

#### start_measures

```cpp
Measures start_measures(int iterations, SensorGroup groups = SensorGroup::All);
```

- `groups` selects which sensor categories to poll. Skipped groups leave their fields at invalid sentinels.
- When `iterations == 1`, the per-iteration delay is skipped and the call returns as soon as I2C reads complete.
- The default `SensorGroup::All` preserves backward compatibility for callers that don't pass a group.

### Tests

- `tests/sensor_manager.tests.cpp`
- `tests/CMakeLists.txt`

## Dependencies

This component depends on these shared components:

- `components/airgradient-common/` for shared data types and RTOS abstraction
- `components/airgradient-serial/` for serial transport abstractions used by multiple sensors
- `components/ads1115/` for the generic ADC helper used by `AlphaSense`

## Build integration

- ESP-IDF builds this component through `components/airgradient-sensors/CMakeLists.txt`
- application code depends on the unified `airgradient-sensors` component instead of the old split sensor components
- the main firmware entrypoint instantiates concrete drivers and passes them into `SensorManager` through the HAL-based `Sensors` struct

Example firmware build from the thin reference product:

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference build
```

## Host tests

Host tests for this component live in `components/airgradient-sensors/tests/`, but they are executed through the root `tests/` runner.

See:

- `tests/README.md` for host-test build and run commands
- `components/airgradient-sensors/tests/CMakeLists.txt` for the component-local host test target wiring

## Adding code here

When adding a new sensor within this component:

- add or extend the public interface in `hal/` if needed
- place the concrete implementation under `drivers/<driver_name>/`
- keep driver-private helpers next to that driver
- update `services/sensor_manager.*` only if the orchestrator must read the new sensor
- update `components/airgradient-sensors/CMakeLists.txt` for new source files
- add or extend host tests under `components/airgradient-sensors/tests/` when the behavior belongs to this component

## Design intent

The goal of this component is clarity, not extra abstraction.

- one place for environmental sensor code
- explicit separation between interfaces, drivers, and orchestration
- minimal impact on existing behavior
- straightforward include paths and build dependencies
