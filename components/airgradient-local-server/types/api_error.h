/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_API_ERROR_H
#define AG_LOCAL_SERVER_API_ERROR_H

#include <cstdint>

#include "types/http_types.h"

// Structured error codes for every request routed to a local-server handler.
// The component composes the error body entirely from its own strings: the
// `code` and `message` come from the helpers below, the `field` (when
// applicable) from the canonical wire key mapped from a ConfigFieldId.
enum class ApiErrorCode : uint8_t {
  InvalidBody,  // malformed / non-object root / trailing garbage -> 400
  UnknownField, // unknown config key                             -> 400
  InvalidValue, // bad type / enum / out of range                 -> 400
  Forbidden,    // rejected by policy / configuration_control lock -> 403
  NotFound,     // action / config field not supported on model    -> 404
  Internal,     // provider / serialize failure                    -> 500
};

// Stable wire string for the `error.code` field.
inline const char *api_error_code_str(ApiErrorCode code) {
  switch (code) {
  case ApiErrorCode::InvalidBody:
    return "invalid_body";
  case ApiErrorCode::UnknownField:
    return "unknown_field";
  case ApiErrorCode::InvalidValue:
    return "invalid_value";
  case ApiErrorCode::Forbidden:
    return "forbidden";
  case ApiErrorCode::NotFound:
    return "not_found";
  case ApiErrorCode::Internal:
    return "internal";
  }
  return "internal";
}

// Standardized human-readable phrase for the `error.message` field.
inline const char *api_error_message(ApiErrorCode code) {
  switch (code) {
  case ApiErrorCode::InvalidBody:
    return "invalid request body";
  case ApiErrorCode::UnknownField:
    return "unknown field";
  case ApiErrorCode::InvalidValue:
    return "invalid value";
  case ApiErrorCode::Forbidden:
    return "forbidden";
  case ApiErrorCode::NotFound:
    return "not found";
  case ApiErrorCode::Internal:
    return "internal error";
  }
  return "internal error";
}

// HTTP status that pairs with each error code (1:1 mapping).
inline HttpStatus api_error_status(ApiErrorCode code) {
  switch (code) {
  case ApiErrorCode::InvalidBody:
  case ApiErrorCode::UnknownField:
  case ApiErrorCode::InvalidValue:
    return HttpStatus::BadRequest;
  case ApiErrorCode::Forbidden:
    return HttpStatus::Forbidden;
  case ApiErrorCode::NotFound:
    return HttpStatus::NotFound;
  case ApiErrorCode::Internal:
    return HttpStatus::InternalServerError;
  }
  return HttpStatus::InternalServerError;
}

#endif // AG_LOCAL_SERVER_API_ERROR_H
