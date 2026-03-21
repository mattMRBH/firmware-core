/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TVOC_NOX_SENSOR_HPP
#define TVOC_NOX_SENSOR_HPP

#include "measures_types.h"

class TVOCNOxSensor {
public:
  virtual ~TVOCNOxSensor() = default;

  virtual bool init() = 0;
  virtual bool read(TVOCNOxData &out) = 0;

private:
};

#endif // !TVOC_NOX_SENSOR_HPP
