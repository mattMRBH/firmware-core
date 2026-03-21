/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef COMMON_H
#define COMMON_H

#include "airgradient_gpio.h"

// Configures pin as output and drives it LOW.
// Returns false if GPIO configuration fails.
bool ext_watchdog_init(const gpio::Hal &hal, int pin);

// Pulses pin HIGH for a fixed 20 ms then back LOW.
void ext_watchdog_reset(const gpio::Hal &hal, int pin);

#endif // COMMON_H
