# ads1115

Reusable ADS1115 / ADS1015 ADC helper used by sensor drivers (e.g.
AlphaSense O3 / NO2 electrochemical front-end). Wraps an upstream library
with ESP-IDF I2C bindings.

## Status

`Stable` — adapted from upstream, minor ESP-IDF integration changes only.

## Scope

This component owns:

- I2C device registration on a caller-initialised `i2c_master_bus_handle_t`
- single-ended and differential ADC reads (channels 0–3)
- gain (`range`), conversion rate, measure mode, and comparator
  configuration helpers
- raw and millivolt result accessors

This component does not own:

- application-level sensor calibration or smoothing
- product-specific ADC channel routing

## Directory Layout

```text
components/ads1115/
  include/
  ads1115.cpp
  CMakeLists.txt
  README.md
```

- `include/ads1115.h` — public C++ class plus enums for `range`,
  `convRate`, `measureMode`, `compMode`, `latch`, etc.
- `ads1115.cpp` — implementation (ESP-IDF I2C master)

## Public Includes

```cpp
#include "ads1115.h"
```

## Design

```text
caller -> ADS1115 instance -> i2c_master -> ADS1115 / ADS1015 chip
```

Configuration is per-conversion (gain, channel, mode); the driver does not
hold long-lived per-channel state.

## Usage

```cpp
ADS1115 adc(i2c_bus, /*address=*/0x48);
if (!adc.init()) { /* handle failure */ }

adc.setVoltageRange_mV(ADS1115_RANGE_4096);
adc.setMeasureMode(ADS1115_SINGLE);
adc.setCompareChannels(ADS1115_COMP_0_GND);

float mv = adc.getResult_mV();
```

See [`include/ads1115.h`](include/ads1115.h) for the full enum set and
method list.

## Dependencies

- `esp_driver_i2c` — I2C master driver
- `esp_timer` — timing helpers used during conversion polling

## Tests

This component does not currently own host tests. ADC-dependent behavior
is exercised via the driver that consumes it (e.g. AlphaSense), which
mocks at a higher level.

## Notes

Adapted from the upstream
[ADS1115 library by Wolfgang (Wolle) Ewald](https://wolles-elektronikkiste.de/en/ads1115-a-d-converter-with-amplifier).
The header retains upstream attribution. Modifications are limited to
ESP-IDF I2C bindings and minor adjustments for the embedded environment.
