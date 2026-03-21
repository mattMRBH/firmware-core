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

private:
};

#endif // !CO2_SENSOR_HPP
