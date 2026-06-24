/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_MEASURES_JSON_H
#define AG_LOCAL_SERVER_MEASURES_JSON_H

#include <cstddef>

#include "measures_types.h"
#include "types/system_info.h"

namespace measures_json {

// Serialize the v1 /api/v1/measures payload into the caller-owned buffer.
// Identity (serial / model / firmware) is always emitted; wifi_rssi is
// emitted only when SystemInfo::wifi_rssi has a value. Each measurement field
// is omitted when invalid (per the Measures is_*_valid() methods) — there is
// no null form, so an unsupported sensor (which reports invalid sentinels)
// is simply absent.
//
// Returns the number of bytes written (excluding the NUL terminator), or 0 on
// failure (buffer too small or cJSON allocation failure).
size_t serialize(const Measures &measures, const SystemInfo &info, char *buf, size_t buf_len);

} // namespace measures_json

#endif // AG_LOCAL_SERVER_MEASURES_JSON_H
