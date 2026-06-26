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

  virtual bool init(bool skip_reset = false) = 0;
  virtual bool read(PMData &out) = 0;

  virtual bool supports_temp_hum() const { return false; }
  virtual TempHumData temp_hum_data() = 0;

  /// Enter/exit low-power mode (stops/restarts the fan). Default no-op for
  /// sensors without a low-power mode. Implementations must be idempotent.
  virtual bool sleep() { return false; }
  virtual bool wake() { return false; }

private:
};
#endif // !PM_SENSOR_HPP
