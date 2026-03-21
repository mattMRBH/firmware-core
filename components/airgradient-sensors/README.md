# Airgradient-Sensors Component

This component owns the shared sensor stack for the firmware.

It keeps the existing architecture intact:

- HAL interfaces for sensor types
- concrete sensor drivers under this component
- `SensorManager` as the shared sensor orchestrator

This is a structural consolidation of the old per-sensor component layout. It does not redefine sensor behavior.

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
- `hal/pm_sensor.h`
- `hal/co2_sensor.h`
- `hal/tvoc_nox_sensor.h`
- `hal/battery_mgmt_sensor.h`
- `hal/o3_no2_sensor.h`

### Drivers

- `drivers/sht40/`
- `drivers/pms5003/`
- `drivers/s8/`
- `drivers/sunlight/`
- `drivers/sgp41/`
- `drivers/bq25xx/`
- `drivers/alpha_sense/`
- `drivers/co2_common/` for the shared Modbus CRC helper used by CO2 drivers

### Services

- `services/sensor_manager.h`
- `services/sensor_manager.cpp`

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

- one place for sensor-facing code
- explicit separation between interfaces, drivers, and orchestration
- minimal impact on existing behavior
- straightforward include paths and build dependencies
