/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BATTERY_MGMT_SENSOR_HPP
#define BATTERY_MGMT_SENSOR_HPP

#include "measures_types.h"

class BatteryMgmtSensor {
public:
  virtual ~BatteryMgmtSensor() = default;

  virtual bool init() = 0;
  virtual bool read(BatteryMgmtData &out) = 0;

private:
};

#endif // !BATTERY_MGMT_SENSOR_HPP
