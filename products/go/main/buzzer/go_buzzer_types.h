/**
 * AirGradient Go -- Buzzer public types
 *
 * Product-specific types shared between the buzzer service, melody
 * library, and orchestrator layers.  No dependencies beyond <cstdint>.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

struct Note {
  uint32_t freq_hz; ///< 0 = silence for the duration
  uint32_t duration_ms;
};
