/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/s8/s8.h"
#include "drivers/co2_common/modbus_crc.h"
#include "esp_log.h"
#include "rtos.h"
#include <cstring>

static constexpr const char *TAG = "S8";

S8::S8(AirgradientSerial &serial) : _serial(serial), _is_calibrating(false) {
  memset(_buf, 0, BUF_SIZE);
}

bool S8::init() {
  // Clear buffer
  while (_serial.available()) {
    _serial.read();
  }

  // Verify communication by reading firmware version
  RTOS::delay_ms(100);

  int16_t version;
  if (!_read_register(FUNC_READ_INPUT_REGISTERS, IR_FW_VERSION, version)) {
    ESP_LOGE(TAG, "Failed to read firmware version");
    return false;
  }

  ESP_LOGI(TAG, "Firmware version: %d.%d", (version >> 8) & 0xFF,
           version & 0xFF);
  ESP_LOGI(TAG, "Sensor initialized, warming up");

  return true;
}

bool S8::read(CO2Data &out) {
  // Initialize to invalid sentinel
  out.co2 = MeasuresInvalid::CO2;

  int16_t co2;
  if (!_read_register(FUNC_READ_INPUT_REGISTERS, IR_SPACE_CO2, co2)) {
    ESP_LOGW(TAG, "Failed to read CO2 value");
    return false;
  }

  out.co2 = static_cast<int>(co2);
  ESP_LOGD(TAG, "CO2: %d ppm", out.co2);

  return true;
}

bool S8::set_baseline_calibration() {
  if (!_clear_acknowledgement()) {
    ESP_LOGE(TAG, "Failed to clear acknowledgement before calibration");
    return false;
  }

  // Send background calibration command
  if (!_write_register(HR_SPECIAL_COMMAND, CMD_BACKGROUND_CALIBRATION)) {
    ESP_LOGE(TAG, "Failed to start baseline calibration");
    return false;
  }

  _is_calibrating = true;
  ESP_LOGI(TAG, "Baseline calibration started");
  return true;
}

bool S8::is_baseline_calibration_done() {
  if (!_is_calibrating) {
    return true;
  }

  int16_t ack = _get_acknowledgement();
  if (ack < 0) {
    return false;
  }

  if (ack & ACK_CO2_BACKGROUND_CALIB) {
    _is_calibrating = false;
    ESP_LOGI(TAG, "Baseline calibration complete");
    return true;
  }

  return false;
}

bool S8::set_abc_period(int hours) {
  if (hours < 0 || hours > 4800) {
    ESP_LOGE(TAG, "Invalid ABC period: %d (must be 0-4800)", hours);
    return false;
  }

  // Check current period
  int current_period = get_abc_period();
  if (current_period == hours) {
    ESP_LOGD(TAG, "ABC period already set to %d hours", hours);
    return true;
  }

  if (!_write_register(HR_ABC_PERIOD, static_cast<uint16_t>(hours))) {
    ESP_LOGE(TAG, "Failed to set ABC period");
    return false;
  }

  ESP_LOGI(TAG, "ABC period set to %d hours", hours);
  return true;
}

int S8::get_abc_period() {
  int16_t period;
  if (!_read_register(FUNC_READ_HOLDING_REGISTERS, HR_ABC_PERIOD, period)) {
    ESP_LOGW(TAG, "Failed to read ABC period");
    return -1;
  }

  ESP_LOGD(TAG, "ABC period: %d hours", period);
  return static_cast<int>(period);
}

void S8::_send_command(uint8_t func, uint16_t reg, uint16_t value) {
  _buf[0] = MODBUS_ADDRESS;
  _buf[1] = func;
  _buf[2] = (reg >> 8) & 0xFF;
  _buf[3] = reg & 0xFF;
  _buf[4] = (value >> 8) & 0xFF;
  _buf[5] = value & 0xFF;

  uint16_t crc = modbus_crc16(_buf, 6);
  _buf[6] = crc & 0xFF;
  _buf[7] = (crc >> 8) & 0xFF;

  _serial.write(_buf, 8);
}

uint8_t S8::_read_bytes(uint8_t max_bytes, uint32_t timeout_ms) {
  uint8_t num_bytes = 0;
  uint64_t start_time = RTOS::get_time_ms();

  while (num_bytes < max_bytes) {
    if (_serial.available()) {
      int byte = _serial.read();
      if (byte >= 0) {
        _buf[num_bytes++] = static_cast<uint8_t>(byte);
      }
    }

    if ((RTOS::get_time_ms() - start_time) >= timeout_ms) {
      break;
    }

    RTOS::delay_ms(1);
  }

  return num_bytes;
}

bool S8::_validate_response(uint8_t func, uint8_t num_bytes,
                            uint8_t expected_len) {
  // Check length if specified
  if (expected_len > 0 && num_bytes != expected_len) {
    ESP_LOGW(TAG, "Invalid response length: %d (expected %d)", num_bytes,
             expected_len);
    return false;
  }

  // Minimum valid response is 5 bytes (addr + func + len + crc_lo + crc_hi)
  if (num_bytes < 5) {
    ESP_LOGW(TAG, "Response too short: %d bytes", num_bytes);
    return false;
  }

  // Validate CRC
  uint16_t received_crc = (_buf[num_bytes - 1] << 8) | _buf[num_bytes - 2];
  uint16_t calculated_crc = modbus_crc16(_buf, num_bytes - 2);

  if (received_crc != calculated_crc) {
    ESP_LOGW(TAG, "CRC mismatch: received 0x%04X, calculated 0x%04X",
             received_crc, calculated_crc);
    return false;
  }

  // Validate address and function code
  if (_buf[0] != MODBUS_ADDRESS) {
    ESP_LOGW(TAG, "Invalid address: 0x%02X", _buf[0]);
    return false;
  }

  if (_buf[1] != func) {
    ESP_LOGW(TAG, "Invalid function code: 0x%02X (expected 0x%02X)", _buf[1],
             func);
    return false;
  }

  // For read commands, validate byte count
  if (func == FUNC_READ_HOLDING_REGISTERS ||
      func == FUNC_READ_INPUT_REGISTERS) {
    uint8_t byte_count = _buf[2];
    if (num_bytes != byte_count + 5) {
      ESP_LOGW(TAG, "Byte count mismatch: %d+5 != %d", byte_count, num_bytes);
      return false;
    }
  }

  return true;
}

bool S8::_read_register(uint8_t func, uint16_t reg, int16_t &value) {
  // Send read command
  _send_command(func, reg, 0x0001);

  // Wait for response
  memset(_buf, 0, BUF_SIZE);
  uint8_t num_bytes = _read_bytes(7, TIMEOUT_MS);

  // Validate response
  if (!_validate_response(func, num_bytes, 7)) {
    return false;
  }

  // Extract value (bytes 3 and 4)
  value = static_cast<int16_t>((_buf[3] << 8) | _buf[4]);
  return true;
}

bool S8::_write_register(uint16_t reg, uint16_t value) {
  uint8_t sentBuf[8];

  // Send write command
  _send_command(FUNC_WRITE_SINGLE_REGISTER, reg, value);

  // Save sent bytes for comparison
  memcpy(sentBuf, _buf, 8);

  // Wait for response (echoes request)
  memset(_buf, 0, BUF_SIZE);
  uint8_t num_bytes = _read_bytes(8, TIMEOUT_MS);

  // Response should match request for successful write
  if (num_bytes != 8 || memcmp(sentBuf, _buf, 8) != 0) {
    ESP_LOGW(TAG, "Write register response mismatch");
    return false;
  }

  return true;
}

bool S8::_clear_acknowledgement() {
  if (!_write_register(HR_ACKNOWLEDGEMENT, 0x0000)) {
    ESP_LOGW(TAG, "Failed to clear acknowledgement");
    return false;
  }

  ESP_LOGD(TAG, "Acknowledgement cleared");
  return true;
}

int16_t S8::_get_acknowledgement() {
  int16_t ack;
  if (!_read_register(FUNC_READ_HOLDING_REGISTERS, HR_ACKNOWLEDGEMENT, ack)) {
    ESP_LOGW(TAG, "Failed to read acknowledgement");
    return -1;
  }

  return ack;
}
