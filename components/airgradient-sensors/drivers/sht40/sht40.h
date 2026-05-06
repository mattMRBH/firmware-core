/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SHT40_HPP
#define SHT40_HPP

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/temp_hum_sensor.h"

/**
 * @brief SHT40 temperature and humidity sensor driver
 *
 * Communicates with sensor using I2C protocol.
 * Supports single-shot measurements with three accuracy levels.
 * All readings are validated using CRC8 checksum.
 */
class SHT40 : public TempHumSensor {
public:
  /**
   * @brief Measurement accuracy levels
   */
  enum class Accuracy {
    HIGH,   ///< Highest precision, 10ms measurement time
    MEDIUM, ///< Medium precision, 4ms measurement time
    LOW     ///< Lowest precision, 2ms measurement time
  };

  /**
   * @brief I2C address options
   */
  static constexpr uint8_t ADDRESS_DEFAULT = 0x44;
  static constexpr uint8_t ADDRESS_ALT = 0x45;

  /**
   * @brief Construct SHT40 sensor with I2C bus
   * @param i2cBus I2C master bus handle
   * @param address I2C address (0x44 or 0x45)
   */
  explicit SHT40(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_DEFAULT);
  virtual ~SHT40() = default;

  // TempHumSensor interface implementation
  bool init() override;
  bool read(TempHumData &out) override;

  /**
   * @brief Set measurement accuracy
   * @param accuracy Desired accuracy level
   * @return true if successful, false otherwise
   */
  [[nodiscard]] bool set_accuracy(Accuracy accuracy);

  /**
   * @brief Get current accuracy setting
   * @return Current accuracy level
   */
  [[nodiscard]] Accuracy get_accuracy() const { return _accuracy; }

private:
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;
  uint8_t _address;
  Accuracy _accuracy;

  // Measurement commands for different accuracy levels
  static constexpr uint8_t CMD_MEASURE_HIGH = 0xFD;
  static constexpr uint8_t CMD_MEASURE_MEDIUM = 0xF6;
  static constexpr uint8_t CMD_MEASURE_LOW = 0xE0;

  // Control commands
  static constexpr uint8_t CMD_RESET = 0x94;
  static constexpr uint8_t CMD_SERIAL = 0x89;

  // Measurement durations (milliseconds)
  static constexpr uint8_t DURATION_HIGH_MS = 10;
  static constexpr uint8_t DURATION_MEDIUM_MS = 5;
  static constexpr uint8_t DURATION_LOW_MS = 2;

  // Initialization timing (milliseconds) - from SHT4x datasheet
  static constexpr uint8_t POWERUP_DELAY_MS = 5;   // Power-up delay before first I2C transaction
  static constexpr uint8_t RESET_DELAY_MS = 25;    // Delay after soft reset
  static constexpr uint8_t APPSTART_DELAY_MS = 10; // Delay before application start
  static constexpr uint8_t TX_RX_DELAY_MS = 10;    // Delay between transmit and receive
  static constexpr uint8_t CMD_TX_DELAY_MS = 1; // Delay after command transmission for bus settling
  static constexpr uint8_t RETRY_DELAY_MS = 10; // Delay between read retry attempts
  static constexpr uint8_t RETRY_MAX = 2;       // Maximum read retry attempts

  // Data size constants
  static constexpr uint8_t DATA_SIZE = 6; // temp_msb, temp_lsb, temp_crc, hum_msb, hum_lsb, hum_crc
  static constexpr uint8_t SERIAL_SIZE = 6; // serial data with CRCs

  bool _reset();

  /**
   * @brief Calculate CRC8 checksum
   * @param data Data buffer
   * @param len Length of data
   * @return CRC8 value
   */
  uint8_t _calculateCrc8(const uint8_t *data, uint8_t len) const;

  /**
   * @brief Get measurement command for current accuracy
   * @return Command byte
   */
  uint8_t _getMeasurementCommand() const;

  /**
   * @brief Get measurement duration for current accuracy
   * @return Duration in milliseconds
   */
  uint8_t _getMeasurementDuration() const;
};

#endif // SHT40_HPP
