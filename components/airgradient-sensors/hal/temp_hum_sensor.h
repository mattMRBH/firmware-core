/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TEMP_HUM_SENSOR_HPP
#define TEMP_HUM_SENSOR_HPP

#include "measures_types.h"

class TempHumSensor {
public:
  virtual ~TempHumSensor() = default;

  virtual bool init() = 0;
  virtual bool read(TempHumData &out) = 0;

private:
};
#endif // !TEMP_HUM_SENSOR_HPP
