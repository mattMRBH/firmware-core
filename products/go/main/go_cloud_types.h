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

#include "go_config_types.h"

/// AgClientResult stored as uint8_t so go_events.h avoids the
/// airgradient-client header dependency.
using CloudResultByte = uint8_t;

struct FetchConfigEventPayload {
  CloudResultByte result = 0;
  GoConfigUpdate update{};
  bool led_test_requested = false;
};

static_assert(std::is_trivially_copyable<FetchConfigEventPayload>::value,
              "Fetch config event payload must be queue-copyable");

#endif // GO_CLOUD_TYPES_H
