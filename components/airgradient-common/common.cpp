/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "common.h"

#include "rtos.h"

static constexpr uint32_t EXT_WDT_PULSE_MS = 20;

bool ext_watchdog_init(const gpio::Hal &hal, int pin) {
  return hal.configure(pin, gpio::Mode::Output, gpio::PullMode::Floating,
                       gpio::InterruptType::Disabled);
}

void ext_watchdog_reset(const gpio::Hal &hal, int pin) {
  hal.set_level(pin, 1);
  RTOS::delay_ms(EXT_WDT_PULSE_MS);
  hal.set_level(pin, 0);
}
