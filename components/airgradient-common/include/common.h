/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef COMMON_H
#define COMMON_H

#include "airgradient_gpio.h"

#include <string>

// Configures pin as output and drives it LOW.
// Returns false if GPIO configuration fails.
bool ext_watchdog_init(const gpio::Hal &hal, int pin);

// Pulses pin HIGH for a fixed 20 ms then back LOW.
void ext_watchdog_reset(const gpio::Hal &hal, int pin);

// Builds a 12-character hex serial number from the Wi-Fi STA MAC address.
// Returns an empty string on failure or when running under TEST_HOST.
std::string build_serial_number();

// Reads the firmware version from the running app description.
// Returns an empty string on failure or when running under TEST_HOST.
std::string build_firmware_version();

#endif // COMMON_H
