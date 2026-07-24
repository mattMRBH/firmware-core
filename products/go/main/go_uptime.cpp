/**
 * AirGradient Go -- retained monotonic uptime
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_uptime.h"

#include <limits>

#ifndef TEST_HOST
#include "esp_attr.h"
#else
#define RTC_DATA_ATTR
#endif

#include "rtos.h"

namespace {

constexpr uint64_t INVALID_START_TIME_MS = std::numeric_limits<uint64_t>::max();
constexpr uint64_t MILLISECONDS_PER_MINUTE = 60'000ULL;

RTC_DATA_ATTR uint64_t s_start_time_ms = INVALID_START_TIME_MS;

} // namespace

void go_uptime_init() {
  const uint64_t now_ms = RTOS::get_retained_time_ms();
  if (s_start_time_ms == INVALID_START_TIME_MS || now_ms < s_start_time_ms) {
    s_start_time_ms = now_ms;
  }
}

uint32_t go_uptime_minutes() {
  const uint64_t now_ms = RTOS::get_retained_time_ms();
  if (s_start_time_ms == INVALID_START_TIME_MS || now_ms < s_start_time_ms) {
    s_start_time_ms = now_ms;
    return 0;
  }

  const uint64_t completed_minutes = (now_ms - s_start_time_ms) / MILLISECONDS_PER_MINUTE;
  if (completed_minutes > std::numeric_limits<uint32_t>::max()) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(completed_minutes);
}

#ifdef TEST_HOST
void go_uptime_reset_retained_state_for_test() { s_start_time_ms = INVALID_START_TIME_MS; }
#endif
