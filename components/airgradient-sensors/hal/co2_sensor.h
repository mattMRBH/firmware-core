/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CO2_SENSOR_HPP
#define CO2_SENSOR_HPP

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

  /// Start a background baseline calibration (400 ppm reference).
  /// Returns true if the command was accepted by the sensor.
  virtual bool set_baseline_calibration() { return false; }

  /// Poll whether a previously started calibration has finished.
  /// Returns true when calibration is complete (or if none was started).
  virtual bool is_baseline_calibration_done() { return true; }

private:
};

#endif // !CO2_SENSOR_HPP
