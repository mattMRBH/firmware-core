/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef S8_HPP
#define S8_HPP

#include <stdint.h>

#include "airgradient_serial.h"
#include "hal/co2_sensor.h"

/**
 * @brief SenseAir S8 CO2 sensor driver
 *
 * Communicates with sensor using Modbus RTU protocol over serial (9600 baud).
 * Supports CO2 reading, automatic baseline calibration (ABC), and manual
 * calibration.
 */
class S8 : public CO2Sensor {
public:
  /**
   * @brief Construct S8 sensor with serial interface
   * @param serial Reference to initialized AirgradientSerial instance
   */
  explicit S8(AirgradientSerial &serial);
  virtual ~S8() = default;

  // CO2Sensor interface implementation
  bool init() override;
  bool read(CO2Data &out) override;
  TempHumData temp_hum_data() override;

  /**
   * @brief Perform manual baseline calibration to 400 PPM
   * @note Sensor must be in clean air environment for 5 minutes before
   * calibration
   * @return true if calibration started successfully, false otherwise
   */
  [[nodiscard]] bool set_baseline_calibration();

  /**
   * @brief Check if baseline calibration is complete
   * @return true if calibration done, false if still in progress
   */
  [[nodiscard]] bool is_baseline_calibration_done();

  /**
   * @brief Set ABC (Automatic Baseline Calibration) period
   * @param hours Calibration period in hours (0 = disable, 4-4800 = enable)
   * @return true if successful, false otherwise
   */
  [[nodiscard]] bool set_abc_period(int hours);

  /**
   * @brief Get current ABC period
   * @return ABC period in hours, -1 on error
   */
  [[nodiscard]] int get_abc_period();

private:
  AirgradientSerial &_serial;

  static constexpr int BAUDRATE = 9600;
  static constexpr int TIMEOUT_MS = 5000;
  static constexpr int BUF_SIZE = 20;

  uint8_t _buf[BUF_SIZE];
  bool _is_calibrating;

  // Modbus function codes
  enum ModbusFunction {
    FUNC_READ_HOLDING_REGISTERS = 0x03,
    FUNC_READ_INPUT_REGISTERS = 0x04,
    FUNC_WRITE_SINGLE_REGISTER = 0x06,
  };

  // Modbus register addresses (Input Registers)
  enum InputRegister {
    IR_METER_STATUS = 0x0000,
    IR_ALARM_STATUS = 0x0001,
    IR_OUTPUT_STATUS = 0x0002,
    IR_SPACE_CO2 = 0x0003,
    IR_PWM_OUTPUT = 0x0015,
    IR_SENSOR_TYPE_ID_HIGH = 0x0019,
    IR_SENSOR_TYPE_ID_LOW = 0x001A,
    IR_MEMORY_MAP_VERSION = 0x001B,
    IR_FW_VERSION = 0x001C,
    IR_SENSOR_ID_HIGH = 0x001D,
    IR_SENSOR_ID_LOW = 0x001E,
  };

  // Modbus register addresses (Holding Registers)
  enum HoldingRegister {
    HR_ACKNOWLEDGEMENT = 0x0000,
    HR_SPECIAL_COMMAND = 0x0001,
    HR_ABC_PERIOD = 0x001F,
  };

  // Special commands
  enum SpecialCommand {
    CMD_BACKGROUND_CALIBRATION = 0x7C06,
    CMD_ZERO_CALIBRATION = 0x7C07,
  };

  // Acknowledgement flags
  enum AckFlags {
    ACK_CO2_BACKGROUND_CALIB = 0x0020,
    ACK_CO2_NITROGEN_CALIB = 0x0040,
  };

  static constexpr uint8_t MODBUS_ADDRESS = 0xFE; // S8 uses any address

  /**
   * @brief Send Modbus command
   * @param func Function code
   * @param reg Register address
   * @param value Value (number of registers to read or value to write)
   */
  void _send_command(uint8_t func, uint16_t reg, uint16_t value);

  /**
   * @brief Read bytes from serial with timeout
   * @param max_bytes Maximum bytes to read
   * @param timeout_ms Timeout in milliseconds
   * @return Number of bytes read
   */
  uint8_t _read_bytes(uint8_t max_bytes, uint32_t timeout_ms);

  /**
   * @brief Validate Modbus response
   * @param func Expected function code
   * @param num_bytes Number of bytes received
   * @param expected_len Expected total length (0 = don't check length)
   * @return true if response valid, false otherwise
   */
  bool _validate_response(uint8_t func, uint8_t num_bytes, uint8_t expected_len = 0);

  /**
   * @brief Read 16-bit register value
   * @param func Function code (FUNC_READ_HOLDING_REGISTERS or
   * FUNC_READ_INPUT_REGISTERS)
   * @param reg Register address
   * @param value Output value
   * @return true if successful, false otherwise
   */
  bool _read_register(uint8_t func, uint16_t reg, int16_t &value);

  /**
   * @brief Write 16-bit register value
   * @param reg Register address
   * @param value Value to write
   * @return true if successful, false otherwise
   */
  bool _write_register(uint16_t reg, uint16_t value);

  /**
   * @brief Clear acknowledgement flags
   * @return true if successful, false otherwise
   */
  bool _clear_acknowledgement();

  /**
   * @brief Get acknowledgement flags
   * @return Acknowledgement flags, -1 on error
   */
  int16_t _get_acknowledgement();
};

#endif // S8_HPP
