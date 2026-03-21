/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_GPIO_H
#define AIRGRADIENT_GPIO_H

// GPIO hardware abstraction layer.
//
// Provides a stateless, namespace-based API. Hardware peripherals are global by nature; this
// component reflects that by using free functions and a function-pointer table (gpio::Hal)
// instead of class instances.
//
// Usage in production code:
//   #include "native_gpio.h"
//   // pass gpio::native::hal wherever a const gpio::Hal & is expected
//
// Usage in tests:
//   Construct a gpio::Hal with mock function pointers; no virtual dispatch required.

namespace gpio {

inline constexpr int INVALID_PIN = -1;

// Interrupt service routine callback. Called from ISR context when a configured interrupt fires.
// The handler must follow ISR-safe rules: no blocking, no heap allocation, no logging.
using InterruptHandler = void (*)(void *arg);

enum class Mode {
  Input,
  Output,
  InputOutput,
};

enum class PullMode {
  Floating,
  PullUp,
  PullDown,
  PullUpDown,
};

enum class InterruptType {
  Disabled,
  RisingEdge,
  FallingEdge,
  AnyEdge,
  LowLevel,
  HighLevel,
};

// Function-pointer table representing a GPIO backend.
//
// gpio::Hal is a plain value type: a struct of function pointers with no state of its own and
// no lifecycle management. Pass by const-reference for dependency injection.
//
// All operations:
//   ISR-safe:    no
//   Thread-safe: no
//   Blocking:    no
//   Allocates:   no
//
// Note: configure() requires all four parameters to be passed explicitly. Function pointers do
// not carry default argument information.
struct Hal {
  bool (*configure)(int pin, Mode mode, PullMode pull, InterruptType interrupt);
  int (*get_level)(int pin);
  bool (*set_level)(int pin, int level);

  // Registers an ISR handler for the pin. handler and arg must remain valid for the lifetime of
  // the interrupt registration. On ESP-IDF the ISR service is installed lazily on first call.
  bool (*add_interrupt_handler)(int pin, InterruptHandler handler, void *arg);
  bool (*remove_interrupt_handler)(int pin);
  bool (*enable_interrupt)(int pin);
  bool (*disable_interrupt)(int pin);
};

} // namespace gpio

#endif // AIRGRADIENT_GPIO_H
