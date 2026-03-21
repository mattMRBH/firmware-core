# airgradient-gps

Generic GPS component providing an abstract HAL interface and an NMEA-based
driver implementation.

## Purpose

Reads NMEA 0183 sentences from an `AirgradientSerial` transport, parses them
via [`libnmea-esp32`](https://github.com/igrr/libnmea-esp32), and exposes
structured GPS data with field-level validation and invalid sentinels.

**Initial target hardware:** Allystar TAU1113 (standard NMEA 0183 output).  
The component is generic enough for any GPS module that outputs NMEA sentences.

## Directory Layout

```
airgradient-gps/
  hal/
    gps_sensor.h          # Abstract GpsSensor interface (no libnmea/serial deps)
  types/
    gps_types.h           # GpsData, GpsFix, GpsPosition, GpsTimestamp,
                          # invalid sentinels, is_*_valid() helpers
  drivers/
    nmea_gps/
      nmea_gps.h          # NmeaGps driver header
      nmea_gps.cpp        # NmeaGps driver implementation
  tests/
    CMakeLists.txt        # Standalone native test build
    gps_component.tests.cpp
    nmea_gps.tests.cpp
  CMakeLists.txt          # ESP-IDF component registration
  README.md               # This file
```

## Dependencies

| Dependency          | Role                                      |
| ------------------- | ----------------------------------------- |
| `airgradient-serial`| Abstract serial transport (`AirgradientSerial`) |
| `libnmea-esp32`     | NMEA 0183 sentence parser (git submodule) |

`libnmea-esp32` lives at `components/libnmea-esp32/` and is added as a git
submodule:

```sh
git submodule add https://github.com/igrr/libnmea-esp32.git components/libnmea-esp32
git submodule update --init --recursive components/libnmea-esp32
```

## Include Patterns

Product code and other components include only the HAL and types headers:

```cpp
#include "hal/gps_sensor.h"   // GpsSensor abstract interface
#include "types/gps_types.h"  // GpsData, validation helpers, sentinels
```

The driver header (`drivers/nmea_gps/nmea_gps.h`) is an implementation detail
and should not be included by consumers of the component.

## Usage

```cpp
// Construct with a concrete AirgradientSerial transport.
NmeaGps gps(my_serial);
gps.begin(9600);

// Call periodically (e.g., every second).
if (gps.read()) {
    GpsData data = gps.get_data();
    if (gps.has_valid_fix()) {
        // use data.position, data.altitude_m, data.fix, data.timestamp
    }
}
```

## What This Component Does NOT Own

- Product-specific GPS hardware wiring (UART port selection, pin assignment)
- Antenna control and antenna power management
- GPS module power management (on/off sequencing)
- GNSS constellation or NMEA message configuration sent to the module
- Any sensor fusion or dead-reckoning on top of raw GPS data

These concerns belong in product BSP code or a higher-level application layer.

## Build and Test

**ESP-IDF (firmware build):**

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference build
```

**Native host tests (standalone):**

```sh
cmake -S components/airgradient-gps/tests \
      -B components/airgradient-gps/tests/build \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build components/airgradient-gps/tests/build
ctest --test-dir components/airgradient-gps/tests/build --output-on-failure
```
