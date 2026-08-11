/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/ota_url.h"

#include <cstdio>
#include <cstring>

#include "device_model.h"

namespace ota_url {

namespace {

bool is_present(const char *value) { return value != nullptr && value[0] != '\0'; }

} // namespace

bool build(const OtaRequest &req, char *out, size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return false;
  }

  // All transports require these fields to address the firmware endpoint.
  if (!is_present(req.serial_number) || !is_present(req.current_firmware) ||
      !is_present(req.http_domain)) {
    return false;
  }

#if defined(CONFIG_AG_DEVICE_MODEL_MAX)
  const int written =
      std::snprintf(out, out_size, "http://%s/sensors/%s/max/firmware.bin?current_firmware=%s",
                    req.http_domain, req.serial_number, req.current_firmware);
#elif defined(CONFIG_AG_DEVICE_MODEL_GO)
  const int written = std::snprintf(
      out, out_size, "http://%s/sensors/airgradient:%s/go/firmware.bin?current_firmware=%s",
      req.http_domain, req.serial_number, req.current_firmware);
#elif defined(CONFIG_AG_DEVICE_MODEL_ONE_OPEN_AIR)
  const int written = std::snprintf(
      out, out_size, "http://%s/sensors/airgradient:%s/generic/os/firmware.bin?current_firmware=%s",
      req.http_domain, req.serial_number, req.current_firmware);
#endif

  // snprintf returns the length it would have written; >= out_size means the
  // URL was truncated.
  if (written < 0 || static_cast<size_t>(written) >= out_size) {
    return false;
  }

  return true;
}

} // namespace ota_url
