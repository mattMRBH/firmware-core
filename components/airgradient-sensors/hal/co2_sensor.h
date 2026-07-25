/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CO2_SENSOR_HPP
#define CO2_SENSOR_HPP

#include <cstdint>

#include "measures_types.h"

class CO2Sensor {
public:
  virtual ~CO2Sensor() = default;

  virtual bool init() = 0;
  virtual bool read(CO2Data &out) = 0;

  virtual bool supports_temp_hum() const { return false; }
  virtual TempHumData temp_hum_data() = 0;

  /// Return true if this sensor supports manual baseline calibration.
  virtual bool supports_calibration() const { return false; }

  /// Start a background baseline calibration.
  /// @param baseline_ppm Reference CO2 concentration in ppm.
  ///        Drivers that do not support a configurable baseline may ignore it.
  /// Returns true if the command was accepted by the sensor.
  virtual bool do_baseline_calibration(int baseline_ppm = 400) {
    (void)baseline_ppm;
    return false;
  }

  /// Poll whether a previously started calibration has finished.
  /// Returns true when calibration is complete (or if none was started).
  virtual bool is_baseline_calibration_done() { return true; }

  /// Return true if this sensor supports configuring its automatic background
  /// calibration period in whole days.
  virtual bool supports_abc_period_configuration() const { return false; }

  /// Set the automatic background calibration period in whole days, or -1 to
  /// disable automatic background calibration. Returns true only when the
  /// sensor accepts and persists the setting.
  virtual bool set_abc_period_days(int days) {
    (void)days;
    return false;
  }

private:
};

#endif // !CO2_SENSOR_HPP
