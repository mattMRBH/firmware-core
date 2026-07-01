/**
 * AirGradient Go -- Buzzer HAL surface
 *
 * Abstract driver interface consumed by BuzzerService.  Does not include
 * ESP-IDF headers.  Tests mock this interface; the LedcBuzzer concrete
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
// Thread-safe: no. BuzzerService serializes access via the worker task.
// Blocking:    yes. LEDC register writes may have side effects.
// Allocates:   no after construction.
class BuzzerDriver {
public:
  virtual ~BuzzerDriver() = default;

  virtual bool init() = 0;

  /// Set the output frequency in Hz. 0 mutes (duty = 0).
  /// Returns false on hardware error.
  virtual bool set_freq(uint32_t freq_hz) = 0;
};
