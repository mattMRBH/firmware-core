/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "aqi.h"

#include <cmath>

namespace aqi {

int pm25_to_us_aqi(float pm25_ugm3) {
  // NaN fails every comparison, so guard it explicitly.
  if (std::isnan(pm25_ugm3) || pm25_ugm3 < 0.0f) {
    return INVALID_AQI;
  }

  // Linear interpolation across the 2024 US EPA PM2.5 breakpoints.
  const float pm = pm25_ugm3;
  if (pm <= 9.0f) {
    return static_cast<int>((50.0f - 0.0f) / (9.0f - 0.0f) * (pm - 0.0f) + 0.0f);
  }
  if (pm <= 35.4f) {
    return static_cast<int>((100.0f - 51.0f) / (35.4f - 9.1f) * (pm - 9.0f) + 51.0f);
  }
  if (pm <= 55.4f) {
    return static_cast<int>((150.0f - 101.0f) / (55.4f - 35.5f) * (pm - 35.5f) + 101.0f);
  }
  if (pm <= 125.4f) {
    return static_cast<int>((200.0f - 151.0f) / (125.4f - 55.5f) * (pm - 55.5f) + 151.0f);
  }
  if (pm <= 225.4f) {
    return static_cast<int>((300.0f - 201.0f) / (225.4f - 125.5f) * (pm - 125.5f) + 201.0f);
  }
  if (pm <= 325.4f) {
    return static_cast<int>((500.0f - 301.0f) / (325.4f - 225.5f) * (pm - 225.5f) + 301.0f);
  }
  return 500;
}

UsAqiCategory us_aqi_to_category(int aqi) {
  if (aqi < 0) {
    return UsAqiCategory::Invalid;
  }
  if (aqi <= 50) {
    return UsAqiCategory::Good;
  }
  if (aqi <= 100) {
    return UsAqiCategory::Moderate;
  }
  if (aqi <= 150) {
    return UsAqiCategory::UnhealthySensitive;
  }
  if (aqi <= 200) {
    return UsAqiCategory::Unhealthy;
  }
  if (aqi <= 300) {
    return UsAqiCategory::VeryUnhealthy;
  }
  return UsAqiCategory::Hazardous;
}

} // namespace aqi
