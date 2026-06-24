/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_SYSTEM_INFO_H
#define AG_LOCAL_SERVER_SYSTEM_INFO_H

#include <optional>

// Device identity and link info embedded in the GET /api/v1/measures
// payload. The Home Assistant integration reads `model` from here to drive
// its model-based field / action / range mapping.
//
// wifi_rssi is optional: std::nullopt when the link quality is unavailable,
// in which case the key is omitted from the measures payload.
struct SystemInfo {
  char serial_number[24] = {};
  char model[32] = {};
  char firmware[16] = {};
  std::optional<int> wifi_rssi; // dBm; omitted when unavailable
};

#endif // AG_LOCAL_SERVER_SYSTEM_INFO_H
