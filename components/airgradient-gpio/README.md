# AirGradient-GPIO Component

This component provides the shared GPIO foundation for AirGradient firmware.

It stays intentionally small and only covers reusable GPIO operations such as:

- configuring a pin as input or output
- reading and writing pin level
- configuring interrupt type
- registering and removing GPIO interrupt handlers
- enabling and disabling GPIO interrupts

This component does not own higher-level button behavior such as debounce,
short press, long press, factory reset, or touch policy. Those behaviors should
live in product-specific code or in higher-level services when needed.

## Directory Layout

```text
components/airgradient-gpio/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` - public GPIO types and `gpio::Hal` function-pointer table
- `drivers/` - native platform GPIO implementation (`gpio::native::hal`)

## Design Direction

GPIO is a global hardware peripheral; no object instance is needed to represent it.
The component uses a namespace-based, stateless API built around `gpio::Hal`:

```text
product code or service -> gpio::native::hal -> free functions -> ESP-IDF driver/gpio
```

`gpio::Hal` is a plain struct of function pointers. It carries no state and requires no
initialization. In production, pass `gpio::native::hal` by const-reference wherever a
`const gpio::Hal &` is expected:

```cpp
#include "native_gpio.h"

bool init_something(const gpio::Hal &hal, int pin) {
  return hal.configure(pin, gpio::Mode::Output,
                       gpio::PullMode::Floating, gpio::InterruptType::Disabled);
}

// at the call site:
init_something(gpio::native::hal, MY_PIN);
```

In tests, construct a `gpio::Hal` directly with mock function pointers. No virtual dispatch
or class inheritance is required.
