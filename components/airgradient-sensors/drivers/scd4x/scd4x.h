/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SCD4X_HPP
#define SCD4X_HPP

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/co2_sensor.h"

/**
 * @brief Sensirion SCD4x CO2 sensor adapter (SCD40 / SCD41 / SCD43)
 *
 * Wraps the shared `embedded-i2c-scd4x` Sensirion driver and adapts it to the
 * `CO2Sensor` interface used by the rest of the firmware. Runs the sensor in
 * periodic-measurement mode (5 s update interval) and exposes the integrated
 * temperature and humidity readings via `supports_temp_hum()` /
 * `temp_hum_data()`.
 *
 * Raw data is already converted to physical units by the Sensirion driver:
 * - CO2: ppm (uint16)
 * - Temperature: milli-degrees Celsius (int32)
 * - Humidity: milli-percent RH (int32)
 *
 * @note Singleton constraint: the underlying `embedded-i2c-scd4x` component
 *       keeps the I2C bus handle and the I2C device address in file-scope
 *       globals. Only one `SCD4x` instance can be used at a time. Constructing
 *       a second instance (even on a different bus) will rebind those globals
 *       and break the first instance.
 */
class SCD4x : public CO2Sensor {
public:
  /**
   * @brief Default I2C address shared by SCD40 / SCD41 / SCD43
   */
  static constexpr uint8_t ADDRESS_DEFAULT = 0x62;

  /**
   * @brief Construct SCD4x sensor adapter with I2C bus
   * @param i2c_bus I2C master bus handle
   * @param address I2C address (default 0x62)
   */
  explicit SCD4x(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_DEFAULT);
  virtual ~SCD4x() = default;

  // CO2Sensor interface implementation
  bool init() override;
  bool read(CO2Data &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  uint8_t _address;
  bool _measuring;

  // Cached last temp/hum reading from the most recent successful read()
  TempHumData _last_temp_hum;

  // I2C probe configuration
  static constexpr int I2C_PROBE_TIMEOUT_MS = 500;

  // Initialization retry parameters
  static constexpr int INIT_PROBE_RETRIES = 3;
  static constexpr int INIT_PROBE_DELAY_MS = 100;
  static constexpr int START_MEASUREMENT_RETRIES = 3;
  static constexpr int START_MEASUREMENT_RETRY_DELAY_MS = 100;

  // Clean-state sequence timings (per Sensirion example)
  static constexpr int WAKE_UP_DELAY_MS = 30;
  static constexpr int STOP_PERIODIC_DELAY_MS = 500;

  // Read retry parameters
  static constexpr int READ_MEASUREMENT_RETRIES = 3;
  static constexpr int READ_MEASUREMENT_RETRY_DELAY_MS = 10;

  // Serial number is 3 words (6 bytes) — Sensirion driver API requires the
  // caller to pass a word buffer and its word length.
  static constexpr uint16_t SERIAL_NUMBER_WORD_COUNT = 3;
};

#endif // SCD4X_HPP
