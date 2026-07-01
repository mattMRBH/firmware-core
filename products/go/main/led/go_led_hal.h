/**
 * AirGradient Go -- LED HAL surface
 *
 * Abstract driver interface consumed by LedService.  Does not include
 * ESP-IDF headers.  Tests mock this interface; the LP5036 concrete
 * implementation is linked only in the firmware build.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

// ISR-safe:    no
// Thread-safe: no. LedService serializes access via the worker task.
// Blocking:    yes. I2C transactions may wait up to timeout.
// Allocates:   no after construction.
class LedDriver {
public:
  virtual ~LedDriver() = default;

  virtual bool init() = 0;
  virtual bool set_channel(uint8_t channel, uint8_t pwm) = 0;
  virtual bool set_rgb(uint8_t b_channel, uint8_t r, uint8_t g, uint8_t b) = 0;

  static constexpr uint8_t NUM_CHANNELS = 36;
};
