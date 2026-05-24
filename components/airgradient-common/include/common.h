/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef COMMON_H
#define COMMON_H

#include "airgradient_gpio.h"

#include <cstdint>
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

// Generates a random decimal number with exactly the requested digit length.
// Returns 0 when the requested length is invalid for uint32_t output.
uint32_t generate_random_number(uint8_t length);

// Reboots the MCU. No-op when running under TEST_HOST.
void reboot();

// Formats a network-byte-order IPv4 address into a dotted-decimal string
// (e.g. 0x0104a8c0 -> "192.168.4.1"). The output buffer must be at least
// 16 bytes (covers "255.255.255.255" + NUL). The caller is responsible
// for ensuring the buffer is large enough.
void format_ipv4_be(uint32_t ip_be, char out[16]);

#endif // COMMON_H
