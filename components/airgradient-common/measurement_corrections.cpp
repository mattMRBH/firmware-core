/**
 * AirGradient measurement correction transforms.
 */

#include "measurement_corrections.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float EPA_PM25_LOW_SLOPE = 0.524f;
constexpr float EPA_PM25_HIGH_SLOPE = 0.786f;
constexpr float EPA_HUMIDITY_COEFFICIENT = 0.0862f;
constexpr float EPA_LOW_INTERCEPT = 5.75f;
constexpr float EPA_HIGH_INTERCEPT = 2.966f;
constexpr float EPA_VERY_HIGH_SLOPE = 0.69f;
constexpr float EPA_QUADRATIC_COEFFICIENT = 8.84e-4f;
constexpr float PM25_LOW_BOUNDARY = 30.0f;
constexpr float PM25_MID_BOUNDARY = 50.0f;
constexpr float PM25_HIGH_BOUNDARY = 210.0f;
constexpr float PM25_VERY_HIGH_BOUNDARY = 260.0f;
constexpr float PM25_INTERPOLATION_SCALE = 0.05f;
constexpr float PM25_HIGH_INTERPOLATION_OFFSET = 1.5f;
constexpr float PM25_VERY_HIGH_INTERPOLATION_SCALE = 0.02f;
constexpr float PM25_VERY_HIGH_INTERPOLATION_OFFSET = 4.2f;

bool is_finite(float value) { return std::isfinite(value); }

bool is_pm25_algorithm_valid(Pm25CorrectionAlgorithm algorithm) {
  switch (algorithm) {
  case Pm25CorrectionAlgorithm::None:
  case Pm25CorrectionAlgorithm::Epa2021:
  case Pm25CorrectionAlgorithm::CustomViaPm25Raw:
    return true;
  }
  return false;
}

bool is_linear_algorithm_valid(LinearCorrectionAlgorithm algorithm) {
  switch (algorithm) {
  case LinearCorrectionAlgorithm::None:
  case LinearCorrectionAlgorithm::Custom:
    return true;
  }
  return false;
}

bool is_raw_temperature_valid(const TempHumData &data) {
  return data.is_temp_valid() && is_finite(data.temperature);
}

bool is_raw_humidity_valid(const TempHumData &data) {
  return data.is_hum_valid() && is_finite(data.humidity);
}

bool is_raw_pm25_valid(const PMData &data) {
  return data.is_pm_25_valid() && is_finite(data.pm_25);
}

float epa_2021_pm25(float pm25, float humidity) {
  if (pm25 == 0.0f) {
    return 0.0f;
  }

  const float clamped_humidity =
      std::clamp(humidity, MeasuresRange::MIN_VALID_HUM, MeasuresRange::MAX_VALID_HUM);
  if (pm25 < PM25_LOW_BOUNDARY) {
    return EPA_PM25_LOW_SLOPE * pm25 - EPA_HUMIDITY_COEFFICIENT * clamped_humidity +
           EPA_LOW_INTERCEPT;
  }

  if (pm25 < PM25_MID_BOUNDARY) {
    const float x = PM25_INTERPOLATION_SCALE * pm25 - PM25_HIGH_INTERPOLATION_OFFSET;
    return (EPA_PM25_HIGH_SLOPE * x + EPA_PM25_LOW_SLOPE * (1.0f - x)) * pm25 -
           EPA_HUMIDITY_COEFFICIENT * clamped_humidity + EPA_LOW_INTERCEPT;
  }

  if (pm25 < PM25_HIGH_BOUNDARY) {
    return EPA_PM25_HIGH_SLOPE * pm25 - EPA_HUMIDITY_COEFFICIENT * clamped_humidity +
           EPA_LOW_INTERCEPT;
  }

  if (pm25 < PM25_VERY_HIGH_BOUNDARY) {
    const float y = PM25_VERY_HIGH_INTERPOLATION_SCALE * pm25 - PM25_VERY_HIGH_INTERPOLATION_OFFSET;
    return (EPA_VERY_HIGH_SLOPE * y + EPA_PM25_HIGH_SLOPE * (1.0f - y)) * pm25 -
           EPA_HUMIDITY_COEFFICIENT * clamped_humidity * (1.0f - y) + EPA_HIGH_INTERCEPT * y +
           EPA_LOW_INTERCEPT * (1.0f - y) + EPA_QUADRATIC_COEFFICIENT * pm25 * pm25 * y;
  }

  return EPA_HIGH_INTERCEPT + EPA_VERY_HIGH_SLOPE * pm25 + EPA_QUADRATIC_COEFFICIENT * pm25 * pm25;
}

float correct_linear_value(float raw, const LinearCorrection &correction, float invalid) {
  if (!is_finite(raw) || !is_finite(correction.scaling_factor) ||
      !is_finite(correction.intercept)) {
    return invalid;
  }

  if (correction.algorithm == LinearCorrectionAlgorithm::None) {
    return raw;
  }

  const float corrected = correction.scaling_factor * raw + correction.intercept;
  if (!is_finite(corrected)) {
    return invalid;
  }
  return corrected;
}

} // namespace

bool is_pm25_correction_valid(const Pm25Correction &correction) {
  return is_pm25_algorithm_valid(correction.algorithm) && is_finite(correction.scaling_factor) &&
         is_finite(correction.intercept);
}

bool is_linear_correction_valid(const LinearCorrection &correction) {
  return is_linear_algorithm_valid(correction.algorithm) && is_finite(correction.scaling_factor) &&
         is_finite(correction.intercept);
}

bool are_measurement_corrections_valid(const MeasurementCorrections &corrections) {
  return is_pm25_correction_valid(corrections.pm25) &&
         is_linear_correction_valid(corrections.temperature) &&
         is_linear_correction_valid(corrections.humidity);
}

bool measurement_corrections_equal(const MeasurementCorrections &lhs,
                                   const MeasurementCorrections &rhs) {
  return lhs.pm25.algorithm == rhs.pm25.algorithm &&
         lhs.pm25.scaling_factor == rhs.pm25.scaling_factor &&
         lhs.pm25.intercept == rhs.pm25.intercept && lhs.pm25.use_epa2021 == rhs.pm25.use_epa2021 &&
         lhs.temperature.algorithm == rhs.temperature.algorithm &&
         lhs.temperature.scaling_factor == rhs.temperature.scaling_factor &&
         lhs.temperature.intercept == rhs.temperature.intercept &&
         lhs.humidity.algorithm == rhs.humidity.algorithm &&
         lhs.humidity.scaling_factor == rhs.humidity.scaling_factor &&
         lhs.humidity.intercept == rhs.humidity.intercept;
}

MeasuresAGo apply_measurement_corrections(const MeasuresAGo &raw,
                                          const MeasurementCorrections &corrections) {
  MeasuresAGo corrected = raw;

  if (!is_raw_temperature_valid(raw.temp_hum_a)) {
    corrected.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  } else {
    const float value = correct_linear_value(raw.temp_hum_a.temperature, corrections.temperature,
                                             MeasuresInvalid::TEMPERATURE);
    corrected.temp_hum_a.temperature = is_finite(value) && value >= MeasuresRange::MIN_VALID_TEMP &&
                                               value <= MeasuresRange::MAX_VALID_TEMP
                                           ? value
                                           : MeasuresInvalid::TEMPERATURE;
  }

  if (!is_raw_humidity_valid(raw.temp_hum_a)) {
    corrected.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  } else {
    const float value = correct_linear_value(raw.temp_hum_a.humidity, corrections.humidity,
                                             MeasuresInvalid::HUMIDITY);
    corrected.temp_hum_a.humidity = is_finite(value) && value >= MeasuresRange::MIN_VALID_HUM &&
                                            value <= MeasuresRange::MAX_VALID_HUM
                                        ? value
                                        : MeasuresInvalid::HUMIDITY;
  }

  if (!is_raw_pm25_valid(raw.pm_a)) {
    corrected.pm_a.pm_25 = MeasuresInvalid::PM;
    return corrected;
  }

  const Pm25Correction &pm25 = corrections.pm25;
  float corrected_pm25 = raw.pm_a.pm_25;
  if (pm25.algorithm == Pm25CorrectionAlgorithm::Epa2021) {
    if (is_raw_humidity_valid(raw.temp_hum_a)) {
      corrected_pm25 = epa_2021_pm25(raw.pm_a.pm_25, raw.temp_hum_a.humidity);
    }
  } else if (pm25.algorithm == Pm25CorrectionAlgorithm::CustomViaPm25Raw) {
    float linear_pm25 = 0.0f;
    if (raw.pm_a.pm_25 != 0.0f) {
      linear_pm25 = pm25.scaling_factor * raw.pm_a.pm_25 + pm25.intercept;
    }

    if (!is_finite(linear_pm25)) {
      corrected.pm_a.pm_25 = MeasuresInvalid::PM;
      return corrected;
    }
    linear_pm25 = std::max(0.0f, linear_pm25);

    corrected_pm25 = linear_pm25;
    if (pm25.use_epa2021) {
      if (!is_raw_humidity_valid(raw.temp_hum_a)) {
        corrected_pm25 = raw.pm_a.pm_25;
      } else {
        corrected_pm25 = epa_2021_pm25(linear_pm25, raw.temp_hum_a.humidity);
      }
    }
  }

  if (!is_finite(corrected_pm25)) {
    corrected.pm_a.pm_25 = MeasuresInvalid::PM;
  } else {
    corrected.pm_a.pm_25 = std::max(0.0f, corrected_pm25);
  }
  return corrected;
}
