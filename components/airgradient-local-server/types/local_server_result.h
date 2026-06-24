/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_RESULT_H
#define AG_LOCAL_SERVER_RESULT_H

#include <cstdint>

// Results are pointer-free: providers return only enums. The component owns
// and serializes all error strings (canonical field name + a standardized
// message per status). This removes any borrowed-string lifetime hazard — a
// provider can never accidentally return a stack/local pointer that dangles
// during error serialization.

// Whether the config resource is exposed and, if so, read-only or read-write.
enum class ConfigAccess : uint8_t { Disabled, ReadOnly, ReadWrite };

// Mirrors the config catalog; the component maps each id to its canonical
// wire key (e.g. CountryCode -> "country") when building an error body. None
// is used when no specific field applies.
enum class ConfigFieldId : uint8_t {
  None,
  CountryCode,
  PmStandard,
  TempUnit,
  CloudEnabled,
  ConfigurationControl,
  Co2CalibDays,
  TvocOffset,
  NoxOffset,
  LedBarMode,
  LedBarBrightness,
  DisplayBrightness,
};

enum class ConfigApplyStatus : uint8_t {
  Ok,           // accepted, persisted, applied        -> 204
  InvalidValue, // out of range / bad enum (semantic)  -> 400
  Forbidden,    // configuration_control gate / policy -> 403
  NotSupported, // field not supported on this model   -> 404
  Internal,     // persistence / apply failure         -> 500
};

struct ConfigApplyResult {
  ConfigApplyStatus status = ConfigApplyStatus::Internal;
  // The offending field for InvalidValue / NotSupported; None otherwise. The
  // component maps it to the canonical wire key in the error body.
  ConfigFieldId field = ConfigFieldId::None;
};

enum class ActionId : uint8_t { CalibrateCo2, TestLeds };

enum class ActionStatus : uint8_t {
  Dispatched,   // accepted and queued (fire-and-forget) -> 200
  Rejected,     // policy / state gate                   -> 403
  NotSupported, // action not available on this model    -> 404
};

struct ActionResult {
  ActionStatus status = ActionStatus::NotSupported;
};

#endif // AG_LOCAL_SERVER_RESULT_H
