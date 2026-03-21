/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef PAYLOAD_CACHE_TYPES_H
#define PAYLOAD_CACHE_TYPES_H

#include <cstdint>

#include "../../airgradient-common/include/measures_types.h"

#ifndef CONFIG_PAYLOAD_CACHE_MAX_SIZE
#define CONFIG_PAYLOAD_CACHE_MAX_SIZE 16
#endif

#if !defined(CONFIG_PAYLOAD_CACHE_TYPE_FULL) &&                                \
    !defined(CONFIG_PAYLOAD_CACHE_TYPE_BASIC)
#define CONFIG_PAYLOAD_CACHE_TYPE_FULL 1
#endif

#if defined(CONFIG_PAYLOAD_CACHE_TYPE_BASIC)
typedef MeasuresBasic PayloadCacheType;
#else
typedef Measures PayloadCacheType;
#endif

constexpr uint16_t PAYLOAD_CACHE_MAX_SIZE = CONFIG_PAYLOAD_CACHE_MAX_SIZE;

static_assert(PAYLOAD_CACHE_MAX_SIZE > 1,
              "PAYLOAD_CACHE_MAX_SIZE must be greater than 1");

struct PayloadCacheStorageData {
  uint16_t head;
  uint16_t tail;
  PayloadCacheType payloads[PAYLOAD_CACHE_MAX_SIZE];
};

#endif // PAYLOAD_CACHE_TYPES_H
