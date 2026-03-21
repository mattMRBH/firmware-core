/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef PM_SENSOR_HPP
#define PM_SENSOR_HPP

#include "measures_types.h"

class PMSensor {
public:
  virtual ~PMSensor() = default;

  virtual bool init() = 0;
  virtual bool read(PMData &out) = 0;

  virtual bool supports_temp_hum() const { return false; }
  virtual TempHumData temp_hum_data() = 0;

private:
};
#endif // !PM_SENSOR_HPP
