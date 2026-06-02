/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_AQI_H
#define AIRGRADIENT_AQI_H

#include <cstdint>

namespace aqi {

/// Sentinel returned by pm25_to_us_aqi() for invalid input.
inline constexpr int INVALID_AQI = -1;

/// PM2.5 (ug/m3) -> US AQI. Uses the 2024 US EPA PM2.5 breakpoints.
/// Returns INVALID_AQI for negative or NaN input.
int pm25_to_us_aqi(float pm25_ugm3);

/// US EPA AQI category bands.
enum class UsAqiCategory : uint8_t {
  Invalid = 0,
  Good = 1,               ///< 0..50
  Moderate = 2,           ///< 51..100
  UnhealthySensitive = 3, ///< 101..150
  Unhealthy = 4,          ///< 151..200
  VeryUnhealthy = 5,      ///< 201..300
  Hazardous = 6,          ///< 301..500
};

/// AQI value -> category. Negative -> Invalid; > 500 -> Hazardous.
UsAqiCategory us_aqi_to_category(int aqi);

} // namespace aqi

#endif // AIRGRADIENT_AQI_H
