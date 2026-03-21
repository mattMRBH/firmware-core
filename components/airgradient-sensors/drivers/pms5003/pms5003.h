/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef PMS5003_HPP
#define PMS5003_HPP

#include "drivers/pms5003/pms5003_base.h"

/**
 * @brief PMS5003 particulate matter sensor driver
 *
 * Supports standard PM readings:
 * - PM 1.0, 2.5, 10.0 (atmospheric environment and standard particle)
 * - Particle counts for 0.3, 0.5, 1.0, 2.5, 5.0, 10.0 μm
 *
 * Does not support temperature and humidity.
 */
class PMS5003 : public PMS5003Base {
public:
  /**
   * @brief Construct PMS5003 sensor with serial interface
   * @param serial Reference to initialized AirgradientSerial instance
   */
  explicit PMS5003(AirgradientSerial &serial);
  virtual ~PMS5003() = default;

  // PMSensor interface implementation
  bool read(PMData &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

private:
};

#endif // PMS5003_HPP
