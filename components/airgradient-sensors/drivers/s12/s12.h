/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef S12_HPP
#define S12_HPP

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/co2_sensor.h"

/**
 * @brief SenseAir S12 CO2 sensor driver (I2C variant)
 *
 * Communicates with the sensor using the ESP-IDF I2C master driver.
 * This iteration exposes only init() and read(); calibration helpers
 * (background / forced) can be added later.
 *
 * The CO2 value is read from a single 16-bit big-endian register pair.
 * By default the "Measured Filtered Pressure Compensated" register
 * (0x06/0x07) is used; a different source register can be provided via
 * the constructor if a product needs the raw or unfiltered variant.
 */
class S12 : public CO2Sensor {
public:
  /** @brief Default 7-bit I2C address. */
  static constexpr uint8_t ADDRESS_DEFAULT = 0x68;

  /**
   * @brief CO2 source register (high byte).
   *
   * 0x06/0x07 = Measured concentration, filtered, pressure compensated.
   * Other S12 register options (raw / unfiltered) follow the same
   * big-endian 16-bit layout and can be selected by passing a different
   * high-byte register to the constructor.
   */
  static constexpr uint8_t REG_CO2_FILT_PRES_COMP_HI = 0x06;

  /**
   * @brief Construct an S12 driver bound to an I2C master bus.
   * @param i2c_bus    Initialized I2C master bus handle.
   * @param address    7-bit device address (default 0x68).
   * @param co2_reg_hi High byte of the 16-bit CO2 register to read
   *                   (default 0x06 = filtered + pressure compensated).
   */
  explicit S12(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_DEFAULT,
               uint8_t co2_reg_hi = REG_CO2_FILT_PRES_COMP_HI);
  virtual ~S12() = default;

  // CO2Sensor interface implementation
  bool init() override;
  bool read(CO2Data &out) override;
  TempHumData temp_hum_data() override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;
  uint8_t _address;
  uint8_t _co2_reg_hi;
  bool _initialized;

  // I2C configuration
  static constexpr uint32_t I2C_CLOCK_HZ = 100000;
  static constexpr int I2C_TIMEOUT_MS = 1000;

  // IO retry parameters (mirrors the reference S12 I2C driver)
  static constexpr int IO_RETRY_COUNT = 5;
  static constexpr int IO_RETRY_DELAY_MS = 10;
  static constexpr int IO_PROBE_TICKLE_MS = 20;

  // Initialization retry parameters
  static constexpr int INIT_PROBE_RETRIES = 3;
  static constexpr int INIT_PROBE_DELAY_MS = 100;

  // CO2 register read width (two data bytes, big-endian ppm).
  static constexpr size_t CO2_REG_READ_LEN = 2;

  /**
   * @brief Read `len` bytes from an S12 register with retry + bus tickle.
   * @param reg Register address (high byte for multi-byte reads).
   * @param buf Output buffer, must hold `len` bytes.
   * @param len Number of bytes to read.
   * @return true on success, false after exhausting retries.
   */
  bool _read_register(uint8_t reg, uint8_t *buf, size_t len);
};

#endif // S12_HPP
