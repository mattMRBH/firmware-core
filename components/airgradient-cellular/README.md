# airgradient-cellular

Shared cellular modem foundation: modem-facing HAL, public types, AT-command
session handling, and concrete modem drivers. This component provides the
bearer that later connectivity components (`airgradient-http`,
`airgradient-mqtt`, `airgradient-coap`) will sit on top of.

## Status

`Scaffold` — directory structure and intent are in place; the HAL,
AT-command service, and the first SIMCOM driver are planned follow-up work.

## Scope

This component owns:

- a public modem HAL for shared cellular capabilities
- shared cellular result and request/response types
- reusable AT-command session handling
- concrete modem drivers and their private helpers
- host-testable parsing and state-machine-like logic where practical

This component does not own:

- AirGradient backend endpoint or payload semantics
- Wi-Fi support
- OTA partition write/apply logic
- bearer-agnostic HTTP abstractions

## Directory Layout

```text
components/airgradient-cellular/
  hal/
  types/
  services/
  drivers/
  tests/
  CMakeLists.txt
  README.md
```

- `hal/` — public modem-facing interfaces shared across products
- `types/` — public enums, structs, and request/response models
- `services/` — shared internals such as the AT-command handler
- `drivers/` — concrete modem drivers grouped by modem family
- `tests/` — host-side tests owned by this component

## Public Includes

```cpp
#include "hal/cellular_modem.h"
#include "types/cellular_types.h"
#include "services/at_command_handler.h"
#include "drivers/simcom_a7672x/simcom_a7672x.h"
```

Guideline:

- include from `hal/` when depending on the shared modem contract
- include from `types/` when using public cellular models
- include from `services/` only for shared low-level helpers
- include from `drivers/` only when instantiating a concrete modem

## Design

```text
caller -> CellularModem& -> SimcomA7672x -> AtCommandHandler -> AirgradientSerial -> modem
```

Higher layers depend on the public HAL; modem-specific code stays in
`drivers/` and never leaks into product code.

## Usage

This component is currently a scaffold; no public driver is exposed yet.
Once the SIMCOM driver lands, callers will create a concrete modem and
pass a `CellularModem &` to higher connectivity components.

## Dependencies

- `components/airgradient-common/` — shared types and RTOS abstraction
- `components/airgradient-gpio/` — power-enable and DTR/RTS pin control
- `components/airgradient-serial/` — UART transport for AT commands

## Tests

Host tests live under `components/airgradient-cellular/tests/` and run
through the [tests runner](../../tests/README.md).

## Notes

Planned follow-up work:

- the `CellularModem` HAL and shared cellular types
- the shared AT-command handler service
- the first SIMCOM modem driver
- registration, DNS, and modem HTTP support
