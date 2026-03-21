# Airgradient-Cellular Component

This component owns shared cellular modem behavior for AirGradient firmware.

It is the modem-facing foundation for later connectivity components such as
`airgradient-http`, `airgradient-mqtt`, and `airgradient-coap`.

## Scope

`airgradient-cellular` should own:

- a public modem HAL for shared cellular capabilities
- shared cellular result and request/response types
- reusable AT-command session handling
- concrete modem drivers and their private helpers
- host-testable parsing and state-machine-like logic where practical

`airgradient-cellular` should not own:

- AirGradient backend endpoint or payload semantics
- WiFi support
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

- `hal/` - public modem-facing interfaces shared across products and components
- `types/` - public enums, structs, and request/response models
- `services/` - shared reusable internals such as AT-command handling
- `drivers/` - concrete modem drivers grouped by modem family
- `tests/` - host-side tests owned by this component

## Public Include Direction

Use includes by role:

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

## Build Integration

- ESP-IDF builds this component through
  `components/airgradient-cellular/CMakeLists.txt`
- host tests for this component live in
  `components/airgradient-cellular/tests/`
- higher layers should depend on the public HAL instead of modem-specific code

## Current Status

This component currently contains only the initial scaffold.

Planned follow-up tasks will add:

- the `CellularModem` HAL and shared cellular types
- the shared AT-command handler service
- the first SIMCOM modem driver
- registration, DNS, and modem HTTP support
