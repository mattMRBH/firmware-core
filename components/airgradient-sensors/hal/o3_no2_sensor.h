/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef O3_NO2_SENSOR_HPP
#define O3_NO2_SENSOR_HPP

#include "measures_types.h"

class O3No2Sensor {
public:
  virtual ~O3No2Sensor() = default;

  virtual bool init() = 0;
  virtual bool read(O3No2Data &out) = 0;

private:
};

#endif // !O3_NO2_SENSOR_HPP
