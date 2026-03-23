/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "measures_types.h"

#include "hal/co2_sensor.h"
#include "hal/o3_no2_sensor.h"
#include "hal/pm_sensor.h"
#include "hal/pressure_sensor.h"
#include "hal/temp_hum_sensor.h"
#include "hal/tvoc_nox_sensor.h"

struct Sensors {
  TempHumSensor *temp_hum;
  CO2Sensor *co2;
  PMSensor *pms_a;
  PMSensor *pms_b;
  TVOCNOxSensor *tvoc_nox;
  O3No2Sensor *o3_no2;
  PressureSensor *pressure;
};

class SensorManager {
public:
  SensorManager(Sensors &sensors);
  ~SensorManager();

  Measures start_measures(int iterations);

private:
  Sensors &_sensors;

  struct AverageMeasuresCounters {
    // TempHum counters
    int temp_a = 0;
    int hum_a = 0;
    int temp_b = 0;
    int hum_b = 0;
    // CO2 counter
    int co2 = 0;
    // PM sensor A counters
    int pm_a_01 = 0, pm_a_25 = 0, pm_a_10 = 0;
    int pm_a_01_sp = 0, pm_a_25_sp = 0, pm_a_10_sp = 0;
    int pm_a_03_pc = 0, pm_a_05_pc = 0, pm_a_01_pc = 0;
    int pm_a_25_pc = 0, pm_a_5_pc = 0, pm_a_10_pc = 0;
    // PM sensor B counters
    int pm_b_01 = 0, pm_b_25 = 0, pm_b_10 = 0;
    int pm_b_01_sp = 0, pm_b_25_sp = 0, pm_b_10_sp = 0;
    int pm_b_03_pc = 0, pm_b_05_pc = 0, pm_b_01_pc = 0;
    int pm_b_25_pc = 0, pm_b_5_pc = 0, pm_b_10_pc = 0;
    // TVOCNOx counters
    int tvoc_index = 0, tvoc_raw = 0;
    int nox_index = 0, nox_raw = 0;
    // O3No2 counters
    int o3_we = 0, o3_ae = 0;
    int no2_we = 0, no2_ae = 0;
    int afe_temp = 0;
    // Pressure counters
    int pressure = 0;
    int altitude = 0;
  };

  // Accumulation methods
  void _accumulate_temp_hum(TempHumData &sum_a, TempHumData &sum_b,
                            AverageMeasuresCounters &counters);
  void _accumulate_co2(CO2Data &sum, AverageMeasuresCounters &counters, TempHumData &temp_hum_sum_a,
                       bool co2_supports_temp_hum);
  void _accumulate_pm_sensor(PMSensor *sensor, PMData &sum, AverageMeasuresCounters &counters,
                             TempHumData &temp_hum_sum_a, TempHumData &temp_hum_sum_b,
                             bool is_sensor_a, bool sensor_supports_temp_hum);
  void _accumulate_tvoc_nox(TVOCNOxData &sum, AverageMeasuresCounters &counters);
  void _accumulate_o3_no2(O3No2Data &sum, AverageMeasuresCounters &counters);
  void _accumulate_pressure(PressureData &sum, AverageMeasuresCounters &counters,
                            TempHumData &temp_hum_sum_a, bool pressure_supports_temp_hum);

  // Averaging methods
  TempHumData _calculate_temp_hum_a_average(const TempHumData &sum,
                                            const AverageMeasuresCounters &counters);
  TempHumData _calculate_temp_hum_b_average(const TempHumData &sum,
                                            const AverageMeasuresCounters &counters);
  CO2Data _calculate_co2_average(const CO2Data &sum, const AverageMeasuresCounters &counters);
  PMData _calculate_pm_average(const PMData &sum, const AverageMeasuresCounters &counters,
                               bool is_sensor_a);
  TVOCNOxData _calculate_tvoc_nox_average(const TVOCNOxData &sum,
                                          const AverageMeasuresCounters &counters);
  O3No2Data _calculate_o3_no2_average(const O3No2Data &sum,
                                      const AverageMeasuresCounters &counters);
  PressureData _calculate_pressure_average(const PressureData &sum,
                                           const AverageMeasuresCounters &counters);
};
#endif // !SENSOR_MANAGER_H
