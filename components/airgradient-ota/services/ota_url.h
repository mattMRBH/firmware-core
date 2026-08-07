/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_URL_H
#define AG_OTA_URL_H

#include <cstddef>

#include "types/ota_types.h"

// AG-server firmware URL builder. Single place that knows the AirGradient URL
// conventions; shared by all pull transports (WiFi now, cellular later).
namespace ota_url {

// Builds the model-specific base firmware URL from req and appends
// ?current_firmware={fw}. Callers may append transport-specific params
// (e.g. cellular &offset=&length=&iccid=). Returns false on truncation or
// missing required fields.
bool build(const OtaRequest &req, char *out, size_t out_size);

} // namespace ota_url

#endif // AG_OTA_URL_H
