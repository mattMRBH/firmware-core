/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef MEASURES_TYPES_H
#define MEASURES_TYPES_H

#include "../../airgradient-bms/types/bms_types.h"

namespace MeasuresRange {
// Temperature & Humidity
constexpr float MIN_VALID_TEMP = -40.0f;
constexpr float MAX_VALID_TEMP = 125.0f;
constexpr float MIN_VALID_HUM = 0.0f;
constexpr float MAX_VALID_HUM = 100.0f;
// PM (all PM values)
constexpr float MIN_VALID_PM = 0.0f;
// CO2
constexpr int MIN_VALID_CO2 = 0;
constexpr int MAX_VALID_CO2 = 10000;
// TVOC & NOx
constexpr int MIN_VALID_TVOC = 0;
constexpr int MIN_VALID_NOX = 0;
// Voltage
constexpr float MIN_VALID_VOLT = 0.0f;
}; // namespace MeasuresRange

namespace MeasuresInvalid {
constexpr float TEMPERATURE = -1000.0f;
constexpr float HUMIDITY = -1.0f;
constexpr float PM = -1.0f;
constexpr int CO2 = -1;
constexpr int TVOC = -1;
constexpr int NOX = -1;
constexpr float VOLT = -1.0f;
} // namespace MeasuresInvalid

struct CO2Data {
  int co2;

  bool is_valid() const {
    return co2 >= MeasuresRange::MIN_VALID_CO2 && co2 <= MeasuresRange::MAX_VALID_CO2;
  }
};

struct TempHumData {
  float temperature;
  float humidity;

  bool is_temp_valid() const {
    return temperature >= MeasuresRange::MIN_VALID_TEMP &&
           temperature <= MeasuresRange::MAX_VALID_TEMP;
  }

  bool is_hum_valid() const {
    return humidity >= MeasuresRange::MIN_VALID_HUM && humidity <= MeasuresRange::MAX_VALID_HUM;
  }

  bool is_valid() const { return is_temp_valid() && is_hum_valid(); }
};

struct PMData {
  // Atmospheric environment
  float pm_01;
  float pm_25;
  float pm_10;

  // Standard particle
  float pm_01_sp;
  float pm_25_sp;
  float pm_10_sp;

  // Particle counts
  float pm_03_pc;
  float pm_05_pc;
  float pm_01_pc;
  float pm_25_pc;
  float pm_5_pc;
  float pm_10_pc;

  // Individual validation methods
  bool is_pm_01_valid() const { return pm_01 >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_25_valid() const { return pm_25 >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_10_valid() const { return pm_10 >= MeasuresRange::MIN_VALID_PM; }

  bool is_pm_01_sp_valid() const { return pm_01_sp >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_25_sp_valid() const { return pm_25_sp >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_10_sp_valid() const { return pm_10_sp >= MeasuresRange::MIN_VALID_PM; }

  bool is_pm_03_pc_valid() const { return pm_03_pc >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_05_pc_valid() const { return pm_05_pc >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_01_pc_valid() const { return pm_01_pc >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_25_pc_valid() const { return pm_25_pc >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_5_pc_valid() const { return pm_5_pc >= MeasuresRange::MIN_VALID_PM; }
  bool is_pm_10_pc_valid() const { return pm_10_pc >= MeasuresRange::MIN_VALID_PM; }

  // Overall validation
  bool is_valid() const {
    return is_pm_01_valid() && is_pm_25_valid() && is_pm_10_valid() && is_pm_01_sp_valid() &&
           is_pm_25_sp_valid() && is_pm_10_sp_valid() && is_pm_03_pc_valid() &&
           is_pm_05_pc_valid() && is_pm_01_pc_valid() && is_pm_25_pc_valid() &&
           is_pm_5_pc_valid() && is_pm_10_pc_valid();
  }
};

struct TVOCNOxData {
  int tvoc_index;
  int tvoc_raw;
  int nox_index;
  int nox_raw;

  bool is_tvoc_index_valid() const { return tvoc_index >= MeasuresRange::MIN_VALID_TVOC; }
  bool is_tvoc_raw_valid() const { return tvoc_raw >= MeasuresRange::MIN_VALID_TVOC; }
  bool is_nox_index_valid() const { return nox_index >= MeasuresRange::MIN_VALID_NOX; }
  bool is_nox_raw_valid() const { return nox_raw >= MeasuresRange::MIN_VALID_NOX; }

  bool is_valid() const {
    return is_tvoc_index_valid() && is_tvoc_raw_valid() && is_nox_index_valid() &&
           is_nox_raw_valid();
  }
};

struct O3No2Data {
  float o3_we;
  float o3_ae;
  float no2_we;
  float no2_ae;
  float afe_temp;

  bool is_o3_working_valid() const { return o3_we >= MeasuresRange::MIN_VALID_VOLT; }
  bool is_o3_auxiliary_valid() const { return o3_ae >= MeasuresRange::MIN_VALID_VOLT; }
  bool is_no2_working_valid() const { return no2_we >= MeasuresRange::MIN_VALID_VOLT; }
  bool is_no2_auxiliary_valid() const { return no2_ae >= MeasuresRange::MIN_VALID_VOLT; }
  bool is_afe_temp_valid() const { return afe_temp >= MeasuresRange::MIN_VALID_VOLT; }

  bool is_valid() const {
    return is_o3_working_valid() && is_o3_auxiliary_valid() && is_no2_working_valid() &&
           is_no2_auxiliary_valid() && is_afe_temp_valid();
  }
};

struct Measures {
  TempHumData temp_hum_a;
  TempHumData temp_hum_b;
  PMData pm_a;
  PMData pm_b;
  CO2Data co2;
  TVOCNOxData tvoc_nox;
  BmsTelemetry power;
  O3No2Data electrode;
};

struct MeasuresBasic {
  TempHumData temp_hum_a;
  PMData pm_a;
  CO2Data co2;
  TVOCNOxData tvoc_nox;
};

#endif // !MEASURES_TYPES_H
