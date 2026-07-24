/**
 * AirGradient Go -- retained monotonic uptime
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_UPTIME_H
#define GO_UPTIME_H

#include <cstdint>

/** Initialize the retained uptime session start when it is not already valid. */
void go_uptime_init();

/** Return completed uptime minutes, saturated to the uint32_t wire range. */
uint32_t go_uptime_minutes();

#ifdef TEST_HOST
/** Simulate reloading the RTC_DATA_ATTR initializer after a non-deep-sleep reset. */
void go_uptime_reset_retained_state_for_test();
#endif

#endif // GO_UPTIME_H
