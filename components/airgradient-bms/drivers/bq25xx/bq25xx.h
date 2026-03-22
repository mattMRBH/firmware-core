/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BQ25XX_HPP
#define BQ25XX_HPP

#include <cstdint>

#include "driver/i2c_master.h"

#include "hal/bms_device.h"

/**
 * @brief BQ25XX battery management IC driver (BQ25672, BQ25798)
 *
 * Communicates with BQ25672/BQ25798 battery charger ICs using I2C protocol.
 * Provides comprehensive battery management functionality including voltage
 * readings, temperature monitoring, current measurement, and charging status.
 * Handles ADC configuration and watchdog timer reset.
 */
class BQ25XX : public BmsDevice {
public:
  /**
   * @brief I2C address options
   */
  static constexpr uint8_t ADDRESS_DEFAULT = 0x6B;

  /**
   * @brief Construct BQ25XX sensor with I2C bus
   * @param i2cBus I2C master bus handle
   * @param address I2C address (default 0x6B)
   */
  explicit BQ25XX(i2c_master_bus_handle_t i2cBus, uint8_t address = ADDRESS_DEFAULT);
  virtual ~BQ25XX() = default;

  // BmsDevice interface implementation
  bool init() override;
  bool read_telemetry(BmsTelemetry &out) override;
  bool read_status(BmsStatus &out) override;
  bool get_battery_percentage(float *output) override;
  bool update_watchdog() override;
  bool feature_ship_available() const override;
  bool enter_ship_mode() override;

  // Voltage readings
  /**
   * @brief Read battery voltage in mV
   * @param output Pointer to store voltage value
   * @return true if successful, false otherwise
   */
  bool getVBAT(uint16_t *output);

  /**
   * @brief Read system voltage in mV
   * @param output Pointer to store voltage value
   * @return true if successful, false otherwise
   */
  bool getVSYS(uint16_t *output);

  /**
   * @brief Read bus (charging) voltage in mV
   * @param output Pointer to store voltage value
   * @return true if successful, false otherwise
   */
  bool getVBUS(uint16_t *output);

  // Battery metrics
  /**
   * @brief Read battery current in mA
   * Handles 2's complement conversion
   * @param output Pointer to store current value (signed)
   * @return true if successful, false otherwise
   */
  bool get_battery_current(int16_t *output);

  /**
   * @brief Read temperature
   * @param output Pointer to store temperature value
   * @return true if successful, false otherwise
   */
  bool get_temperature(float *output);

  /**
   * @brief Get current charging status
   * @return BmsChargingState enum value
   */
  BmsChargingState get_charging_status();

  // Raw ADC readings
  /**
   * @brief Read raw VBAT ADC value (2 bytes)
   * @param output Pointer to store raw value
   * @return true if successful, false otherwise
   */
  bool getVBATRaw(uint16_t *output);

  /**
   * @brief Read raw VSYS ADC value (2 bytes)
   * @param output Pointer to store raw value
   * @return true if successful, false otherwise
   */
  bool getVSYSRaw(uint16_t *output);

  /**
   * @brief Read raw temperature ADC value (2 bytes)
   * @param output Pointer to store raw value
   * @return true if successful, false otherwise
   */
  bool getTemperatureRaw(uint16_t *output);

  /**
   * @brief Read raw VBUS ADC value with fallback logic
   * Tries ADC1 first, falls back to ADC2 if value < 1000
   * @param output Pointer to store raw value
   * @return true if successful, false otherwise
   */
  bool getVBUSRaw(uint16_t *output);

  /**
   * @brief Read raw battery current ADC value (2 bytes)
   * @param output Pointer to store raw value
   * @return true if successful, false otherwise
   */
  bool getIBATRaw(uint16_t *output);

  // Debug/diagnostic methods
  /**
   * @brief Print system status registers for debugging
   * Logs System, Fault, Charge, Input status, and MPPT registers
   */
  void printSystemStatus();

  /**
   * @brief Print control and configuration registers for debugging
   * Logs voltage/current limits and ADC control settings
   */
  void printControlAndConfiguration();

private:
  i2c_master_bus_handle_t _i2cBus;
  i2c_master_dev_handle_t _devHandle;
  uint8_t _address;

  // Watchdog timer tracking
  uint64_t _lastUpdateTimeMs;

  // Register addresses - Control and configuration
  static constexpr uint8_t REG00_MINIMAL_SYSTEM_VOLTAGE = 0x00;
  static constexpr uint8_t REG01_CHARGE_VOLTAGE_LIMIT = 0x01;
  static constexpr uint8_t REG03_CHARGE_CURRENT_LIMIT = 0x03;
  static constexpr uint8_t REG05_INPUT_VOLTAGE_LIMIT = 0x05;
  static constexpr uint8_t REG06_INPUT_CURRENT_LIMIT = 0x06;

  // Register addresses - Status
  static constexpr uint8_t REG_SYSTEM_STATUS = 0x0A;
  static constexpr uint8_t REG_FAULT_STATUS = 0x0B;
  static constexpr uint8_t REG_CHARGE_STATUS = 0x0C;
  static constexpr uint8_t REG_INPUT_STATUS = 0x0D;

  // Register addresses - MPPT and ADC control
  static constexpr uint8_t REG_MPPT = 0x15;
  static constexpr uint8_t REG_CHG_STAT = 0x1C;
  static constexpr uint8_t REG_ADC_CTRL = 0x2E;

  // Register addresses - ADC readings
  static constexpr uint8_t REG_IBAT_ADC = 0x33;
  static constexpr uint8_t REG_VBUS_ADC1 = 0x37;
  static constexpr uint8_t REG_VBUS_ADC2 = 0x39;
  static constexpr uint8_t REG_VBAT_ADC = 0x3B;
  static constexpr uint8_t REG_VSYS_ADC = 0x3D;
  static constexpr uint8_t REG_TEMP_ADC = 0x41;

  // Configuration values
  static constexpr uint8_t MPPT_ENABLE_VALUE = 0xAB;
  static constexpr uint8_t ADC_ENABLE_VALUE = 0x80; // ADC_EN=1, 15-bit, Continuous
  static constexpr uint32_t WATCHDOG_UPDATE_INTERVAL_MS = 10000;
  static constexpr uint16_t VBUS_ADC1_THRESHOLD = 1000; // Minimum valid VBUS ADC1 value

  /**
   * @brief Write single byte to register
   * @param reg Register address
   * @param value Value to write
   * @return true if successful, false otherwise
   */
  bool _writeRegister(uint8_t reg, uint8_t value);

  /**
   * @brief Read register value (1 or 2 bytes)
   * @param reg Register address
   * @param size Number of bytes to read (1 or 2)
   * @param output Pointer to store result
   * @return true if successful, false otherwise
   */
  bool _readRegister(uint8_t reg, uint8_t size, uint16_t *output);
};

#endif // BQ25XX_HPP
