/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PAYLOAD_SERIALIZER_H
#define AG_PAYLOAD_SERIALIZER_H

#include <cstddef>

#include "../types/client_types.h"

namespace ag_client {

// Serialize a Measures snapshot to the AirGradient HTTP JSON format.
//
// Only fields that pass the corresponding `is_*_valid()` method on each
// Measures substruct are included.  Dual-channel fields (full Measures only)
// are averaged when both channels are valid, otherwise the valid channel is
// used.  The "wifi" property (signal) is always included.
//
// Writes a NUL-terminated JSON string into `out` and sets *bytes_written to
// the string length (excluding NUL).
//
// Returns true on success, false if cJSON allocation fails or the buffer is
// too small for the rendered JSON (in which case *bytes_written is 0).
bool serialize_measures_json(const AgClientMeasuresType &measures, int signal, char *out,
                             size_t out_size, size_t *bytes_written);

} // namespace ag_client

#endif // AG_PAYLOAD_SERIALIZER_H
