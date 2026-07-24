/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_SYSTEM_INFO_H
#define AG_LOCAL_SERVER_SYSTEM_INFO_H

#include <cstdint>
#include <optional>

// Device identity, link info, and uptime embedded in the GET /api/v1/measures
// payload. The Home Assistant integration reads `model` from here to drive its
// model-based field / action / range mapping.
//
// wifi_rssi is optional: std::nullopt when the link quality is unavailable,
// in which case the key is omitted from the measures payload.
struct SystemInfo {
  char serial_number[24] = {};  // "serialNumber"
  char model[32] = {};          // "model"
  char firmware[16] = {};       // "firmware"
  std::optional<int> wifi_rssi; // "wifiRssi" (dBm; omitted when unavailable)
  uint32_t boot = 0;            // "boot": saturated uptime in completed minutes
};

#endif // AG_LOCAL_SERVER_SYSTEM_INFO_H
