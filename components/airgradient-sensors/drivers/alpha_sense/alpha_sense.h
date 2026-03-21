/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef ALPHASENSE_HPP
#define ALPHASENSE_HPP

#include <stdint.h>

#include <memory>

#include "driver/i2c_master.h"

#include "ads1115.h"
#include "hal/o3_no2_sensor.h"

/**
 * @brief AlphaSense O3 and NO2 electrochemical sensor driver
 *
 * Uses two ADS1115 16-bit ADCs to read voltage from O3 and NO2 electrodes:
 * - ADS1 (gas): 4 channels for O3 and NO2 working/auxiliary electrodes
 * - ADS2 (temp): 1 channel for analog front-end (AFE) temperature
 *
 * Electrode mapping:
 * - NO2 Working Electrode:    ADS1 Channel 0
 * - NO2 Auxiliary Electrode:  ADS1 Channel 1
 * - O3 Working Electrode:     ADS1 Channel 2
 * - O3 Auxiliary Electrode:   ADS1 Channel 3
 * - AFE Temperature:          ADS2 Channel 0
 */
class AlphaSense : public O3No2Sensor {
public:
  /**
   * @brief Construct AlphaSense sensor with I2C bus and ADC addresses
   * @param i2cBus I2C master bus handle (must be initialized)
   * @param gasAddr I2C address of gas ADC (ADS1115) - default: 0x48
   * @param tempAddr I2C address of temperature ADC (ADS1115) - default: 0x49
   */
  explicit AlphaSense(i2c_master_bus_handle_t i2c_bus, uint8_t gas_addr = 0x48,
                      uint8_t temp_addr = 0x49);
  virtual ~AlphaSense() = default;

  // O3No2Sensor interface implementation
  bool init() override;
  bool read(O3No2Data &out) override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  uint8_t _gas_addr;
  uint8_t _temp_addr;

  std::unique_ptr<ADS1115> _ads_gas;  // Gas sensor ADC (O3/NO2 electrodes)
  std::unique_ptr<ADS1115> _ads_temp; // Temperature sensor ADC (AFE temp)

  /**
   * @brief Read voltage from specific ADC channel
   * @param adc Pointer to ADS1115 instance
   * @param channel Channel to read (ADS1115_COMP_X_GND)
   * @return Voltage in millivolts, -1.0f on error
   */
  float _read_channel(ADS1115 *adc, ADS1115_MUX channel);
};

#endif // ALPHASENSE_HPP
