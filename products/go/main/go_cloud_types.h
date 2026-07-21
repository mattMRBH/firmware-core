/**
 * AirGradient Go — CloudService public types
 *
 * Standalone event-payload header for go_events.h.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_CLOUD_TYPES_H
#define GO_CLOUD_TYPES_H

#include <cstdint>
#include <type_traits>

#include "measurement_corrections.h"

/// AgClientResult stored as uint8_t so go_events.h avoids the
/// airgradient-client header dependency.
using CloudResultByte = uint8_t;

enum class GoConfigField : uint32_t {
  Pm25Correction = 1U << 0,
  TemperatureCorrection = 1U << 1,
  HumidityCorrection = 1U << 2,
};

inline bool has_go_config_field(uint32_t mask, GoConfigField field) {
  return (mask & static_cast<uint32_t>(field)) != 0;
}

struct GoCloudConfigUpdate {
  uint32_t update_mask = 0;
  MeasurementCorrections corrections{};
};

struct FetchConfigEventPayload {
  CloudResultByte result = 0;
  GoCloudConfigUpdate update{};
};

static_assert(std::is_trivially_copyable<GoCloudConfigUpdate>::value,
              "Go cloud updates must be queue-copyable");
static_assert(std::is_trivially_copyable<FetchConfigEventPayload>::value,
              "Fetch config event payload must be queue-copyable");

#endif // GO_CLOUD_TYPES_H
