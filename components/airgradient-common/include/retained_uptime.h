/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_RETAINED_UPTIME_H
#define AG_RETAINED_UPTIME_H

#include <cstdint>

namespace retained_uptime {

/** Initialize the retained uptime session start when it is not already valid. */
void init();

/** Return completed uptime minutes, saturated to the uint32_t maximum. */
uint32_t completed_minutes();

#ifdef TEST_HOST
/** Simulate reloading the RTC_DATA_ATTR initializer after a non-retained reset. */
void reset_state_for_test();
#endif

} // namespace retained_uptime

#endif // AG_RETAINED_UPTIME_H
