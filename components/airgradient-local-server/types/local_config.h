/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_LOCAL_CONFIG_H
#define AG_LOCAL_SERVER_LOCAL_CONFIG_H

#include <optional>
#include <string>

// Flat configuration schema for GET / PUT /api/v1/config. Every field is
// optional both on the wire and here: a device emits only the fields its
// model supports (GET) and applies only the present supported fields (PUT).
// Fields are named by function, not by product. The component owns this
// catalog as a union of known fields; adding a future field (including a
// product-specific one) is a non-breaking addition of one optional field.
struct LocalServerConfig {
  std::optional<std::string> country;               // "country"
  std::optional<std::string> pm_standard;           // "pm_standard"
  std::optional<std::string> temp_unit;             // "temp_unit"
  std::optional<bool> cloud_enabled;                // "cloud_enabled"
  std::optional<std::string> configuration_control; // "configuration_control"
  std::optional<int> co2_calib_days;                // "co2_calib_days"
  std::optional<int> tvoc_offset;                   // "tvoc_offset"
  std::optional<int> nox_offset;                    // "nox_offset"
  std::optional<std::string> led_bar_mode;          // "led_bar_mode"
  std::optional<int> led_bar_brightness;            // "led_bar_brightness"
  std::optional<int> display_brightness;            // "display_brightness"
  // Product-specific fields (for example buzzer_enabled, gps_interval_s) are
  // added here as flat optional fields when a product exposes them over HTTP.
};

#endif // AG_LOCAL_SERVER_LOCAL_CONFIG_H
