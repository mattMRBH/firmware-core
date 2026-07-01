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

// Pull in Kconfig values when building with ESP-IDF.  sdkconfig.h is placed
// in the build/config directory (on the -I path) but is not force-included by
// the toolchain, so components that gate behaviour on Kconfig symbols must
// include it explicitly.  The __has_include guard makes this a no-op for
// native host-test builds where no sdkconfig.h exists.
#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_PAYLOAD_CACHE_MAX_SIZE
#define CONFIG_PAYLOAD_CACHE_MAX_SIZE 16
#endif

#if !defined(CONFIG_PAYLOAD_CACHE_TYPE_FULL) && !defined(CONFIG_PAYLOAD_CACHE_TYPE_BASIC) &&       \
    !defined(CONFIG_PAYLOAD_CACHE_TYPE_AGO)
#define CONFIG_PAYLOAD_CACHE_TYPE_FULL 1
#endif

#if defined(CONFIG_PAYLOAD_CACHE_TYPE_BASIC)
typedef MeasuresBasic PayloadCacheType;
#elif defined(CONFIG_PAYLOAD_CACHE_TYPE_AGO)
typedef MeasuresAGo PayloadCacheType;
#else
typedef Measures PayloadCacheType;
#endif

constexpr uint16_t PAYLOAD_CACHE_MAX_SIZE = CONFIG_PAYLOAD_CACHE_MAX_SIZE;

static_assert(PAYLOAD_CACHE_MAX_SIZE > 1, "PAYLOAD_CACHE_MAX_SIZE must be greater than 1");

struct PayloadCacheStorageData {
  uint16_t head;
  uint16_t tail;
  PayloadCacheType payloads[PAYLOAD_CACHE_MAX_SIZE];
};

#endif // PAYLOAD_CACHE_TYPES_H
