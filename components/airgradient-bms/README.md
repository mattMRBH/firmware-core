# airgradient-bms

Shared battery management (BMS) stack: a HAL for charger / PMIC devices,
public BMS data types, and concrete charger drivers. Decoupled from the
environmental sensor stack (`airgradient-sensors`).

## Status

`Stable`.

## Scope

This component owns:

- the `BmsDevice` HAL interface
- public BMS types: telemetry, status, charging state, power source
- sentinel values and field-level validation helpers for BMS data
- concrete charger / PMIC driver implementations

This component does not own:

- product-level power policy, watchdog cadence, sleep decisions, or
  shutdown behavior
- environmental sensing (lives in `airgradient-sensors`)

## Directory Layout

```text
components/airgradient-bms/
  hal/
  types/
  drivers/
  tests/
  CMakeLists.txt
  README.md
```

- `hal/` — `BmsDevice` abstract interface
- `types/` — `BmsTelemetry`, `BmsStatus`, `BmsChargingState`,
  `BmsPowerSource`, `BmsInvalid` sentinels, and inline `_str` helpers
- `drivers/` — concrete BMS driver implementations grouped by IC family
- `tests/` — host-side tests owned by this component

## Public Includes

```cpp
#include "hal/bms_device.h"
#include "types/bms_types.h"
#include "drivers/bq25xx/bq25xx.h"
#include "drivers/bq25629/bq25629_bms.h"
```

Guideline:

- include from `hal/` when depending on the abstract interface
- include from `types/` when depending on BMS data types only
- include from `drivers/` only when instantiating a concrete driver

## Design

```text
caller -> BmsDevice& -> Bq25xx | Bq25629Bms -> i2c_master -> charger IC
```

Public BMS types live in `types/` and never carry ESP-IDF symbols, so
host tests can construct and validate them without the i2c master.

### BmsDevice virtuals

| Method | Purpose |
|---|---|
| `init()` | Initialize hardware |
| `read_telemetry(out)` | ADC snapshot (voltages, currents, temps) |
| `read_status(out)` | Charging state + power source + fault flags |
| `get_charging_state(state)` | Lightweight single-register read |
| `get_battery_percentage(output)` | SOC estimate (0-100%) |
| `update_watchdog()` | Reset HW watchdog |
| `feature_ship_available()` | Query ship-mode capability |
| `enter_ship_mode()` | Power off (should not return) |
| `set_pmid_enabled(enabled)` | Enable/disable PMID boost converter (EN_OTG) |
| `set_charge_enable(enabled)` | Enable/disable battery charging current path |
| `set_charge_current_ma(mA)` | Set fast-charge current limit (CC mode) |
| `set_watchdog_timeout_ms(ms)` | Configure chip-level watchdog timeout |

The `set_pmid_enabled` primitive is consumed by
`PowerService::set_pm_power()` under the demand-coupled PMID model:
PMID enable tracks PM-sensor demand, not USB plug state.

### Drivers

| Driver | IC | Notes |
|---|---|---|
| `drivers/bq25xx` | TI BQ25672 / BQ25798 | Multi-cell charger family |
| `drivers/bq25629` | TI BQ25629 | Single-cell charger; wraps the vendor `bq25629` component |

## Usage

```cpp
Bq25629Bms bms(i2c_bus);
if (!bms.init()) { /* handle init failure */ }

BmsTelemetry telemetry;
if (bms.read_telemetry(telemetry) && is_vbat_valid(telemetry.vbat_mv)) {
    // forward to power policy
}

BmsStatus status;
bms.read_status(status);
```

See [`drivers/bq25629/bq25629_bms.h`](drivers/bq25629/bq25629_bms.h) and
[`drivers/bq25xx/bq25xx.h`](drivers/bq25xx/bq25xx.h) for full APIs.

## Dependencies

- `components/airgradient-common/` — RTOS abstraction
- `components/bq25629/` — BQ25629 vendor driver wrapped by the
  single-cell adapter
- `esp_driver_i2c` — I2C master bus access (concrete drivers only)

There is no dependency on `airgradient-sensors`.

## Tests

Host tests live under `components/airgradient-bms/tests/` and run through
the [tests runner](../../tests/README.md). Current coverage focuses on
sentinel and validation behavior of the public BMS types.
