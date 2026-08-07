/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_DEVICE_MODEL_H
#define AG_DEVICE_MODEL_H

#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#if !defined(CONFIG_AG_DEVICE_MODEL_ONE_OPEN_AIR) && !defined(CONFIG_AG_DEVICE_MODEL_MAX) &&       \
    !defined(CONFIG_AG_DEVICE_MODEL_GO) && defined(TEST_HOST)
#define CONFIG_AG_DEVICE_MODEL_ONE_OPEN_AIR 1
#endif

#if (defined(CONFIG_AG_DEVICE_MODEL_ONE_OPEN_AIR) + defined(CONFIG_AG_DEVICE_MODEL_MAX) +          \
     defined(CONFIG_AG_DEVICE_MODEL_GO)) != 1
#error "Exactly one AirGradient device model must be selected"
#endif

#endif // AG_DEVICE_MODEL_H
