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

  /// Run one conditioning / warmup cycle.
  ///
  /// Drivers that need an initial conditioning period (e.g. SGP41) should
  /// override this to send their conditioning command. The default is a
  /// no-op that succeeds so sensors without a conditioning phase can ignore
  /// it. Callers invoke this repeatedly during the warmup window.
  virtual bool run_conditioning() { return true; }

private:
};

#endif // !TVOC_NOX_SENSOR_HPP
