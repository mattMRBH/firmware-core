#include "measurement_corrections.h"

#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

MeasuresAGo valid_measures() {
  MeasuresAGo measures{};
  measures.temp_hum_a.temperature = 20.0f;
  measures.temp_hum_a.humidity = 50.0f;
  measures.pm_a.pm_01 = 1.0f;
  measures.pm_a.pm_25 = 10.0f;
  measures.pm_a.pm_10 = 20.0f;
  measures.co2.co2 = 500;
  measures.tvoc_nox.tvoc_index = 100;
  measures.tvoc_nox.nox_index = 50;
  measures.pressure.pressure = 1000.0f;
  return measures;
}

} // namespace

TEST_CASE("identity correction preserves valid measurements and unrelated fields", "[correction]") {
  MeasuresAGo raw = valid_measures();
  raw.power.battery_voltage = 3.7f;

  const MeasuresAGo corrected = apply_measurement_corrections(raw, MeasurementCorrections{});

  REQUIRE(corrected.temp_hum_a.temperature == raw.temp_hum_a.temperature);
  REQUIRE(corrected.temp_hum_a.humidity == raw.temp_hum_a.humidity);
  REQUIRE(corrected.pm_a.pm_25 == raw.pm_a.pm_25);
  REQUIRE(corrected.pm_a.pm_01 == raw.pm_a.pm_01);
  REQUIRE(corrected.power.battery_voltage == raw.power.battery_voltage);
  REQUIRE(corrected.co2.co2 == raw.co2.co2);
}

TEST_CASE("invalid raw target fields become their invalid sentinels", "[correction]") {
  MeasuresAGo raw = valid_measures();
  raw.temp_hum_a.temperature = std::numeric_limits<float>::quiet_NaN();
  raw.temp_hum_a.humidity = std::numeric_limits<float>::infinity();
  raw.pm_a.pm_25 = std::numeric_limits<float>::infinity();

  const MeasuresAGo corrected = apply_measurement_corrections(raw, MeasurementCorrections{});

  REQUIRE(corrected.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
  REQUIRE(corrected.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
  REQUIRE(corrected.pm_a.pm_25 == MeasuresInvalid::PM);
}

TEST_CASE("custom linear corrections apply before range validation", "[correction]") {
  MeasuresAGo raw = valid_measures();
  MeasurementCorrections corrections{};
  corrections.temperature.algorithm = LinearCorrectionAlgorithm::Custom;
  corrections.temperature.scaling_factor = 1.5f;
  corrections.temperature.intercept = -2.0f;
  corrections.humidity.algorithm = LinearCorrectionAlgorithm::Custom;
  corrections.humidity.scaling_factor = 0.5f;
  corrections.humidity.intercept = 10.0f;

  MeasuresAGo corrected = apply_measurement_corrections(raw, corrections);
  REQUIRE(corrected.temp_hum_a.temperature == Catch::Approx(28.0f));
  REQUIRE(corrected.temp_hum_a.humidity == Catch::Approx(35.0f));

  corrections.temperature.intercept = 200.0f;
  corrected = apply_measurement_corrections(raw, corrections);
  REQUIRE(corrected.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
}

TEST_CASE("custom PM correction handles zero, clamp, and EPA ordering", "[correction]") {
  MeasuresAGo raw = valid_measures();
  MeasurementCorrections corrections{};
  corrections.pm25.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  corrections.pm25.scaling_factor = 2.0f;
  corrections.pm25.intercept = 5.0f;

  MeasuresAGo corrected = apply_measurement_corrections(raw, corrections);
  REQUIRE(corrected.pm_a.pm_25 == Catch::Approx(25.0f));

  raw.pm_a.pm_25 = 0.0f;
  corrected = apply_measurement_corrections(raw, corrections);
  REQUIRE(corrected.pm_a.pm_25 == 0.0f);

  corrections.pm25.intercept = -100.0f;
  raw.pm_a.pm_25 = 10.0f;
  corrected = apply_measurement_corrections(raw, corrections);
  REQUIRE(corrected.pm_a.pm_25 == 0.0f);

  raw.pm_a.pm_25 = 10.0f;
  corrections.pm25.intercept = 0.0f;
  corrections.pm25.scaling_factor = 1.0f;
  corrections.pm25.use_epa2021 = true;
  corrected = apply_measurement_corrections(raw, corrections);
  REQUIRE(corrected.pm_a.pm_25 == Catch::Approx(0.524f * 10.0f - 0.0862f * 50.0f + 5.75f));
}

TEST_CASE("EPA correction uses exact branch boundaries and raw humidity", "[correction][epa]") {
  MeasurementCorrections corrections{};
  corrections.pm25.algorithm = Pm25CorrectionAlgorithm::Epa2021;

  for (const float pm25 : {29.999f, 30.0f, 49.999f, 50.0f, 209.999f, 210.0f, 259.999f, 260.0f}) {
    MeasuresAGo raw = valid_measures();
    raw.pm_a.pm_25 = pm25;
    raw.temp_hum_a.humidity = 0.0f;
    const MeasuresAGo corrected = apply_measurement_corrections(raw, corrections);
    REQUIRE(corrected.pm_a.pm_25 >= 0.0f);
  }

  MeasuresAGo raw = valid_measures();
  raw.pm_a.pm_25 = 10.0f;
  raw.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  const MeasuresAGo corrected = apply_measurement_corrections(raw, corrections);
  REQUIRE(corrected.pm_a.pm_25 == raw.pm_a.pm_25);
}

TEST_CASE("correction configuration validation rejects invalid enums and coefficients",
          "[correction][validation]") {
  MeasurementCorrections corrections{};
  REQUIRE(are_measurement_corrections_valid(corrections));

  corrections.pm25.scaling_factor = std::numeric_limits<float>::infinity();
  REQUIRE_FALSE(are_measurement_corrections_valid(corrections));

  corrections = MeasurementCorrections{};
  corrections.temperature.algorithm = static_cast<LinearCorrectionAlgorithm>(99);
  REQUIRE_FALSE(are_measurement_corrections_valid(corrections));
}
