/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef PMS5003T_HPP
#define PMS5003T_HPP

#include "drivers/pms5003/pms5003_base.h"

/**
 * @brief PMS5003T particulate matter sensor with temperature and humidity
 *
 * Supports:
 * - PM 1.0, 2.5, 10.0 (atmospheric environment and standard particle)
 * - Particle counts for 0.3, 0.5, 1.0, 2.5 μm
 * - Temperature and humidity readings
 *
 * Note: PMS5003T frame format differs from PMS5003 in particle count fields.
 */
class PMS5003T : public PMS5003Base {
public:
  /**
   * @brief Construct PMS5003T sensor with serial interface
   * @param serial Reference to initialized AirgradientSerial instance
   */
  explicit PMS5003T(AirgradientSerial &serial);
  virtual ~PMS5003T() = default;

  // PMSensor interface implementation
  bool read(PMData &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

private:
  TempHumData _last_temp_hum;
};

#endif // PMS5003T_HPP
