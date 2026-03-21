/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef NATIVE_GPIO_H
#define NATIVE_GPIO_H

#include "airgradient_gpio.h"

namespace gpio {
namespace native {

// Pre-built gpio::Hal backed by the ESP-IDF GPIO driver.
//
// Use this in product composition code wherever a const gpio::Hal & is expected. No class
// instance or initialization is required.
//
// Example:
//   #include "native_gpio.h"
//   init_something(gpio::native::hal, pin);
extern const Hal hal;

} // namespace native
} // namespace gpio

#endif // NATIVE_GPIO_H
