/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_CLIENT_TYPES_H
#define AG_CLIENT_TYPES_H

#include "../../airgradient-common/include/measures_types.h"

// Pull in Kconfig values when building with ESP-IDF.  See payload_cache_types.h
// for the same pattern; sdkconfig.h is on the -I path under ESP-IDF and absent
// on the native host-test build.
#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#if !defined(CONFIG_AG_CLIENT_MEASURES_TYPE_FULL) &&                                               \
    !defined(CONFIG_AG_CLIENT_MEASURES_TYPE_BASIC) && !defined(CONFIG_AG_CLIENT_MEASURES_TYPE_AGO)
#define CONFIG_AG_CLIENT_MEASURES_TYPE_FULL 1
#endif

#if defined(CONFIG_AG_CLIENT_MEASURES_TYPE_BASIC)
typedef MeasuresBasic AgClientMeasuresType;
#elif defined(CONFIG_AG_CLIENT_MEASURES_TYPE_AGO)
typedef MeasuresAGo AgClientMeasuresType;
#else
typedef Measures AgClientMeasuresType;
#endif

enum class NetworkType {
  Wifi,
  Cellular,
};

enum class AgClientResult {
  Ok,             // Operation succeeded (HTTP 200, 201, or 429)
  BufferTooSmall, // Response did not fit in caller's buffer
  TransportError, // Could not reach server (connection, DNS, timeout)
  ServerError,    // Non-success HTTP status (generic)
  NotRegistered,  // Server returned 400 -- device not registered
};

#endif // AG_CLIENT_TYPES_H
