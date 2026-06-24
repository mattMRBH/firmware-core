/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_CONFIG_JSON_H
#define AG_LOCAL_SERVER_CONFIG_JSON_H

#include <cstddef>
#include <cstdint>

#include "types/local_config.h"
#include "types/local_server_result.h"

namespace config_json {

// Maximum length (including NUL) of an unknown key echoed back in an
// unknown_field error. Longer keys are truncated.
constexpr size_t MAX_UNKNOWN_KEY = 48;

enum class ParseStatus : uint8_t {
  Ok,
  InvalidBody,  // malformed JSON / non-object root / trailing garbage -> 400
  UnknownField, // a key not in the catalog                            -> 400
  InvalidValue, // wrong type or bad enum for a known key              -> 400
};

struct ParseResult {
  ParseStatus status = ParseStatus::InvalidBody;
  // Offending known field for InvalidValue; None otherwise.
  ConfigFieldId field = ConfigFieldId::None;
  // Offending key (NUL-terminated, possibly truncated) for UnknownField.
  char unknown_key[MAX_UNKNOWN_KEY] = {};
};

// Strict full-body parse of a partial config PUT into `out` (only present
// known keys are set). Rejects malformed JSON, a non-object root, trailing
// non-whitespace after the root, unknown keys, and wrong type / enum for a
// known key. The body buffer is not assumed NUL-terminated.
ParseResult parse(const char *body, size_t len, LocalServerConfig &out);

// Serialize the present fields of `cfg` into the caller-owned buffer for a
// GET /api/v1/config response. Absent (std::nullopt) fields are omitted.
// Returns bytes written (excluding NUL), or 0 on failure.
size_t serialize(const LocalServerConfig &cfg, char *buf, size_t buf_len);

// Canonical wire key for a catalog field id, or nullptr for None. Used by the
// component when composing an error body.
const char *config_field_wire_key(ConfigFieldId id);

} // namespace config_json

#endif // AG_LOCAL_SERVER_CONFIG_JSON_H
