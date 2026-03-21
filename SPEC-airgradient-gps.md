# Spec: `airgradient-gps` Component

## Overview

A shared GPS component providing an abstract GPS HAL interface and a generic
NMEA-based driver implementation. The driver reads NMEA sentences from an
`AirgradientSerial` transport, parses them via `libnmea-esp32`, and exposes
structured GPS data with field-level validation and invalid sentinels.

**Initial target hardware:** Allystar TAU1113 (standard NMEA 0183 output).

**Design:** Generic enough to work with any GPS module that outputs standard
NMEA sentences.

## Architecture

```
Product code -> GpsSensor& (hal/) -> NmeaGps (drivers/) -> AirgradientSerial + libnmea
```

This follows the same layered pattern as `airgradient-cellular/` and
`airgradient-sensors/`:

- `hal/` contains the abstract interface — no libnmea or serial includes
- `drivers/` contains the concrete implementation bridging serial I/O to parsed
  GPS data
- `types/` contains public data types, enums, and validation helpers
- `tests/` contains component-local native host tests

## Vendor Dependency

`libnmea-esp32` is added as a git submodule under `components/libnmea-esp32/`
(same pattern as `esp-nimble-cpp/`).

```sh
git submodule add https://github.com/igrr/libnmea-esp32.git components/libnmea-esp32
```

This provides the `libnmea` ESP-IDF component with parsers for: GGA, GLL, RMC,
GSA, GSV, TXT, VTG.

## Directory Structure

```
components/airgradient-gps/
  hal/
    gps_sensor.h                  # Abstract GPS HAL interface
  types/
    gps_types.h                   # Public data types, enums, sentinels, validation
  drivers/
    nmea_gps/
      nmea_gps.h                  # Generic NMEA GPS driver header
      nmea_gps.cpp                # Generic NMEA GPS driver implementation
  tests/
    CMakeLists.txt                # Component-local native test build
    gps_component.tests.cpp       # Scaffold/sentinel tests
    nmea_gps.tests.cpp            # Driver unit tests
  CMakeLists.txt                  # ESP-IDF component registration
  README.md                       # Component documentation
```

## CMakeLists.txt

```cmake
idf_component_register(
    SRCS "drivers/nmea_gps/nmea_gps.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-serial libnmea-esp32
)
```

- `INCLUDE_DIRS "."` — role-based includes: `#include "hal/gps_sensor.h"`,
  `#include "types/gps_types.h"`
- `REQUIRES` lists `airgradient-serial` (serial transport) and `libnmea-esp32`
  (NMEA parsing)

## Types (`types/gps_types.h`)

### Enums

```cpp
enum class GpsFixType : uint8_t {
    NoFix = 0,
    Fix2D = 2,
    Fix3D = 3,
};
```

### Data Structures

```cpp
struct GpsPosition {
    double latitude;    // decimal degrees, positive = North, negative = South
    double longitude;   // decimal degrees, positive = East, negative = West
};

struct GpsFix {
    GpsFixType fix_type;
    int satellite_count;
    float hdop;
    float pdop;
    float vdop;
};

struct GpsTimestamp {
    int year;       // full year (e.g., 2026)
    int month;      // 1-12
    int day;        // 1-31
    int hour;       // 0-23
    int minute;     // 0-59
    int second;     // 0-59
    bool valid;
};

struct GpsData {
    GpsPosition position;
    float altitude_m;       // meters above mean sea level
    GpsFix fix;
    GpsTimestamp timestamp;
};
```

### Invalid Sentinels

```cpp
static constexpr double GPS_LATITUDE_INVALID        = 91.0;      // valid: [-90, 90]
static constexpr double GPS_LONGITUDE_INVALID       = 181.0;     // valid: [-180, 180]
static constexpr float  GPS_ALTITUDE_INVALID        = -10000.0f;
static constexpr int    GPS_SATELLITE_COUNT_INVALID  = -1;
static constexpr float  GPS_DOP_INVALID             = -1.0f;
```

### Field-Level Validation

Free functions following the `is_<field>_valid()` pattern used by
`airgradient-sensors`:

```cpp
bool is_latitude_valid(double lat);
bool is_longitude_valid(double lon);
bool is_position_valid(const GpsPosition& pos);
bool is_altitude_valid(float alt);
bool is_satellite_count_valid(int count);
bool is_fix_valid(const GpsFix& fix);
bool is_gps_timestamp_valid(const GpsTimestamp& ts);
```

## HAL Interface (`hal/gps_sensor.h`)

```cpp
class GpsSensor {
public:
    virtual ~GpsSensor() = default;

    /// Initialize the GPS sensor with the given baud rate.
    virtual bool begin(int baud_rate) = 0;

    /// Shut down the GPS sensor.
    virtual void end() = 0;

    /// Process all available serial data and update internal state.
    /// Returns true if at least one valid NMEA sentence was parsed.
    virtual bool read() = 0;

    /// Get the latest GPS data snapshot.
    virtual GpsData get_data() const = 0;

    /// Check if the current fix is valid (2D or 3D).
    virtual bool has_valid_fix() const = 0;
};
```

**Design notes:**

- Pure virtual interface — no libnmea or serial includes in this header
- `read()` is non-blocking: it processes whatever bytes are currently available
  from the serial buffer
- Products call `read()` periodically (e.g., every second) and then query
  `get_data()` / `has_valid_fix()`
- `begin()` takes baud rate via parameter (constructor injection for hardware
  config, `begin()` for runtime init — consistent with `AirgradientSerial`
  pattern)

## Driver (`drivers/nmea_gps/nmea_gps.h`)

```cpp
class NmeaGps : public GpsSensor {
public:
    explicit NmeaGps(AirgradientSerial& serial);
    ~NmeaGps() override;

    bool begin(int baud_rate) override;
    void end() override;
    bool read() override;
    GpsData get_data() const override;
    bool has_valid_fix() const override;

private:
    AirgradientSerial& serial_;

    // NMEA sentence accumulation buffer
    static constexpr size_t BUFFER_SIZE = 256;
    char buffer_[BUFFER_SIZE];
    size_t buffer_pos_ = 0;

    // Current parsed GPS state (initialized to invalid sentinels)
    GpsData data_;

    // Internal methods
    bool process_byte(char byte);
    void handle_sentence(char* sentence, size_t length);
    void process_gga(const nmea_gpgga_s* gga);
    void process_rmc(const nmea_gprmc_s* rmc);
    void process_gsa(const nmea_gpgsa_s* gsa);

    /// Convert libnmea position (degrees + decimal minutes + cardinal)
    /// to signed decimal degrees.
    static double to_decimal_degrees(const nmea_position& pos);
};
```

### Parsing Flow (`read()`)

1. Read all available bytes from `serial_.available()` / `serial_.read()`
2. For each byte, call `process_byte()`:
   - Accumulate into `buffer_`
   - On `$`, reset buffer position (start of new sentence)
   - On `\n` after `\r`, we have a complete sentence — call
     `handle_sentence()`
   - If buffer overflows, discard and reset
3. `handle_sentence()` calls `nmea_parse()` from libnmea
4. Based on `data->type`, dispatch to `process_gga()`, `process_rmc()`, or
   `process_gsa()`
5. Each processor extracts relevant fields and updates `data_` with validation
6. Call `nmea_free()` to release the parsed struct

### Sentence Handling

| Sentence | Fields Extracted                            | Data Updated                                                       |
| -------- | ------------------------------------------- | ------------------------------------------------------------------ |
| GGA      | lat, lon, altitude, satellite count, fix    | `position`, `altitude_m`, `fix.satellite_count`, `fix.fix_type`    |
| RMC      | lat, lon, date, time, validity              | `position` (if GGA unavailable), `timestamp`                       |
| GSA      | fix type, HDOP, PDOP, VDOP                  | `fix.fix_type`, `fix.hdop`, `fix.pdop`, `fix.vdop`                |

### Coordinate Conversion (`to_decimal_degrees`)

libnmea gives `nmea_position { int degrees; double minutes; char cardinal; }`.
Convert to signed decimal degrees: `degrees + minutes / 60.0`, negated for S/W.

## Tests (`tests/`)

### Test CMake (`tests/CMakeLists.txt`)

Follows the `airgradient-cellular/tests/CMakeLists.txt` pattern:

- Standalone native CMake build
- FetchContent for Catch2 v3.5.0 and Trompeloeil v47
- Compiles production sources under `TEST_HOST` define
- Includes `airgradient-serial/airgradient_serial.cpp` as a dependency for mock
  base class
- Builds the libnmea C parser sources natively with the same
  `COMPILE_DEFINITIONS` rename trick that the ESP-IDF CMakeLists.txt uses

### Mock

```cpp
class MockAirgradientSerial : public AirgradientSerial {
    // Trompeloeil mock or manual stub with rx queue
    // queue_rx(const char* data) to simulate GPS NMEA stream
};
```

### Test Cases (`nmea_gps.tests.cpp`)

1. **Scaffold:** Component compiles and links under `TEST_HOST`
2. **GGA parsing:** Feed valid GGA sentence, verify position, altitude,
   satellite count
3. **RMC parsing:** Feed valid RMC sentence, verify position, date/time
4. **GSA parsing:** Feed valid GSA sentence, verify fix type and DOP values
5. **Multi-sentence accumulation:** Feed GGA + RMC + GSA sequence, verify all
   fields populated
6. **No fix:** Feed GGA with fix indicator 0, verify `has_valid_fix()` returns
   false
7. **Invalid checksum:** Feed sentence with bad checksum, verify data unchanged
8. **Partial sentence:** Feed incomplete sentence, verify no crash and no state
   change
9. **Buffer overflow:** Feed oversized sentence (> 82 chars NMEA max), verify
   graceful discard
10. **Sentinel initialization:** Verify `get_data()` returns all-invalid
    sentinels before any `read()`
11. **Coordinate conversion:** Verify `to_decimal_degrees()` for N/S/E/W
    cardinal directions

### Reference NMEA Test Sentences

```
$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*5B\r\n
$GPRMC,092725.00,A,4717.11364,N,00833.91565,E,0.004,77.52,091202,,,A*57\r\n
$GPGSA,A,3,23,29,07,08,09,18,26,28,,,,,1.94,1.01,1.65*0A\r\n
```

## Integration with Top-Level Tests

The component-local `tests/CMakeLists.txt` supports standalone builds. It should
also be wirable into the top-level `tests/` entrypoint via
`add_subdirectory()` if the project adopts that pattern.

## README.md Content

Document:

- Component purpose and scope (generic NMEA GPS, initial target TAU1113)
- Directory layout (`hal/`, `types/`, `drivers/`, `tests/`)
- Dependency on `airgradient-serial` and `libnmea-esp32`
- Include patterns: `#include "hal/gps_sensor.h"`,
  `#include "types/gps_types.h"`
- What the component does NOT own (product-specific GPS wiring, antenna control,
  power management)
- Build and test instructions

## Implementation Order

1. Add `libnmea-esp32` as a git submodule at `components/libnmea-esp32/`
2. Create `types/gps_types.h` — data structures, sentinels, validation
3. Create `hal/gps_sensor.h` — abstract interface
4. Create `drivers/nmea_gps/nmea_gps.h` and `.cpp` — NMEA driver
5. Create `CMakeLists.txt` — ESP-IDF component registration
6. Create `tests/CMakeLists.txt` and test files — native host tests
7. Create `README.md`
8. Verify ESP-IDF build: `idf.py -C products/reference build`
9. Verify native tests: `cmake && build && ctest`
