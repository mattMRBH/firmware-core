/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/sunlight/sunlight.h"

#include <cstring>

#include "drivers/co2_common/modbus_crc.h"
#include "esp_log.h"
#include "soc/gpio_num.h"

#include "measures_types.h"
#include "rtos.h"

static constexpr const char *TAG = "Sunlight";

Sunlight::Sunlight(AirgradientSerial &serial, MeasurementMode mode, gpio_num_t io_power,
                   int measurement_period_seconds)
    : _serial(serial), _io_power(io_power), _is_calibrating(false), _measurement_triggered(false),
      _last_trigger_time(0), _current_mode(mode),
      _measurement_period_seconds(measurement_period_seconds) {
  memset(_request, 0, BUF_SIZE);
  memset(_response, 0, BUF_SIZE);
  memset(_values, 0, BUF_SIZE * sizeof(uint16_t));
}

bool Sunlight::init() {
  // Clear serial buffer to remove stale data
  while (_serial.available() > 0) {
    _serial.read();
  }

  // Print device ID (non-critical, don't fail on error)
  if (!print_device_id()) {
    ESP_LOGW(TAG, "Failed to read device identification");
  }

  // Read all configuration registers in one command (mode, period, samples)
  // HR_MEASUREMENT_MODE (0x000A), HR_MEASUREMENT_PERIOD (0x000B),
  // HR_MEASUREMENT_SAMPLES (0x000C)
  if (!_read_registers(FUNC_READ_HOLDING_REGISTERS, HR_MEASUREMENT_MODE, 3)) {
    ESP_LOGE(TAG, "Failed to communicate with Sunlight sensor");
    return false;
  }

  // Extract current hardware configuration
  uint16_t current_mode = _values[0];
  uint16_t current_period = _values[1];
  uint16_t current_samples = _values[2];

  // Print current hardware configuration
  ESP_LOGI(TAG, "=== Current Hardware Configuration ===");
  ESP_LOGI(TAG, "  Measurement Mode: %s (0x%04X)",
           (current_mode == MODE_SINGLE) ? "single" : "continuous", current_mode);
  ESP_LOGI(TAG, "  Measurement Period: %d seconds", current_period);
  ESP_LOGI(TAG, "  Measurement Samples: %d", current_samples);

  // Save expected values from constructor BEFORE updating cached
  bool configuration_changed = false;
  MeasurementMode expected_mode = _current_mode;
  uint16_t expected_period = _measurement_period_seconds;

  // Update cached mode with actual hardware value
  _current_mode = static_cast<MeasurementMode>(current_mode);

  // Check and configure measurement mode
  if (current_mode != static_cast<uint16_t>(expected_mode)) {
    ESP_LOGW(TAG, "Mode mismatch: hardware=%s, expected=%s - auto-correcting",
             (current_mode == MODE_SINGLE) ? "single" : "continuous",
             (expected_mode == MODE_SINGLE) ? "single" : "continuous");

    // Write new mode
    uint16_t mode_value = static_cast<uint16_t>(expected_mode);
    if (!_write_registers(HR_MEASUREMENT_MODE, 1, &mode_value)) {
      ESP_LOGE(TAG, "Failed to write measurement mode");
      return false;
    }

    _current_mode = expected_mode;
    configuration_changed = true;
    ESP_LOGI(TAG, "Measurement mode changed to %s",
             (expected_mode == MODE_SINGLE) ? "single" : "continuous");
  }

  // Check and configure measurement period (continuous mode only)
  if (_current_mode == MODE_CONTINUOUS) {
    if (current_period != expected_period) {
      ESP_LOGW(TAG, "Period mismatch: hardware=%d, expected=%d - auto-correcting", current_period,
               expected_period);

      // Write new period
      if (!_write_registers(HR_MEASUREMENT_PERIOD, 1, &expected_period)) {
        ESP_LOGE(TAG, "Failed to write measurement period");
        return false;
      }

      _measurement_period_seconds = expected_period;
      configuration_changed = true;
      ESP_LOGI(TAG, "Measurement period changed to %d seconds", expected_period);
    }
  }

  // Power cycle if any configuration was changed
  if (configuration_changed) {
    ESP_LOGI(TAG, "Configuration changed, power cycling sensor...");
    if (!power_cycle()) {
      ESP_LOGW(TAG, "Power cycle failed or not available");
      // Don't fail init if power cycle not available
    }
  }

  return true;
}

bool Sunlight::read(CO2Data &out) {
  // Initialize to invalid sentinel
  out.co2 = MeasuresInvalid::CO2;

  // In single-shot mode, trigger NEXT measurement at the START
  if (_current_mode == MODE_SINGLE) {
    uint64_t current_time = RTOS::get_time_ms();
    bool can_trigger =
        !_measurement_triggered || (current_time - _last_trigger_time) >= TRIGGER_COOLDOWN_MS;

    if (can_trigger) {
      if (trigger_measurement()) {
        _measurement_triggered = true;
        _last_trigger_time = current_time;
        ESP_LOGI(TAG, "Triggered next measurement");
        return false;
      } else {
        ESP_LOGW(TAG, "Failed to trigger measurement");
      }
    }
  }

  // Read error status and CO2 value atomically (IR 0x0000 to 0x0003 = 4
  // registers) Register layout: [0]=error_status, [1]=alarm_status,
  // [2]=output_status, [3]=CO2
  if (!_read_registers(FUNC_READ_INPUT_REGISTERS, IR_ERROR_STATUS, 4)) {
    ESP_LOGE(TAG, "Failed to read sensor registers");
    return false;
  }

  // Extract values
  int error_status = (int)_values[0];
  int co2_value = (int)_values[3];
  ESP_LOGD(TAG, "Error status: 0x%04X, CO2: %d", error_status, co2_value);

  // Handle critical errors
  if (error_status & ERR_FATAL) {
    ESP_LOGE(TAG, "Fatal sensor error detected");
    return false;
  }
  if (error_status & ERR_CALIBRATION) {
    ESP_LOGW(TAG, "Calibration error detected");
    return false;
  }

  // If no measurement available yet, return false
  if (error_status & ERR_NO_MEASUREMENT) {
    ESP_LOGI(TAG, "Measurement in progress, not ready yet");
    return false;
  }

  // Log other non-critical errors
  if (error_status & ERR_I2C) {
    ESP_LOGW(TAG, "I2C error detected");
  }
  if (error_status & ERR_ALGORITHM) {
    ESP_LOGW(TAG, "Algorithm error detected");
  }
  if (error_status & ERR_SELFDIAG) {
    ESP_LOGW(TAG, "Self-diagnostic error detected");
  }
  if (error_status & ERR_OUT_OF_RANGE) {
    ESP_LOGW(TAG, "Out of range error detected");
  }
  if (error_status & ERR_MEMORY) {
    ESP_LOGW(TAG, "Memory error detected");
  }

  // Return the measurement
  out.co2 = co2_value;
  return true;
}

TempHumData Sunlight::temp_hum_data() {
  // Sunlight does not support temperature and humidity
  TempHumData data;
  data.temperature = MeasuresInvalid::TEMPERATURE;
  data.humidity = MeasuresInvalid::HUMIDITY;
  return data;
}

bool Sunlight::set_baseline_calibration() {
  // Check if sensor is readable before starting calibration
  CO2Data test_data;
  if (!read(test_data)) {
    ESP_LOGE(TAG, "Cannot read sensor, calibration aborted");
    return false;
  }

  // Check error status
  int error_status = _get_error_status();
  if (error_status < 0) {
    ESP_LOGE(TAG, "Failed to read error status, calibration aborted");
    return false;
  }

  // Clear calibration status
  if (!_clear_calibration_status()) {
    ESP_LOGE(TAG, "Failed to clear calibration status");
    return false;
  }

  // Start calibration by writing command to HR2
  uint16_t calib_cmd = CMD_BACKGROUND_CALIBRATION;
  if (!_write_registers(HR_SPECIAL_COMMAND, 1, &calib_cmd)) {
    ESP_LOGE(TAG, "Failed to start calibration");
    return false;
  }

  ESP_LOGI(TAG, "Baseline calibration started");
  _is_calibrating = true;

  return true;
}

bool Sunlight::is_baseline_calibration_done() {
  if (!_is_calibrating) {
    return true; // Not calibrating
  }

  // Check error status for calibration errors
  int error_status = _get_error_status();
  if (error_status >= 0 && (error_status & ERR_CALIBRATION)) {
    ESP_LOGE(TAG, "Calibration error detected");
    _is_calibrating = false;
    return false;
  }

  // Check calibration status register
  int cal_status = _get_calibration_status();
  if (cal_status < 0) {
    ESP_LOGW(TAG, "Failed to read calibration status");
    return false;
  }

  if (cal_status & CAL_STATUS_BACKGROUND_DONE) {
    ESP_LOGI(TAG, "Calibration complete");
    _is_calibrating = false;
    return true;
  }

  // Still calibrating
  return false;
}

bool Sunlight::set_abc_period(int hours) {
  // Read current ABC period
  if (!_read_registers(FUNC_READ_HOLDING_REGISTERS, HR_ABC_PERIOD, 1)) {
    ESP_LOGE(TAG, "Failed to read ABC period");
    return false;
  }

  if (_values[0] == (uint16_t)hours) {
    ESP_LOGI(TAG, "ABC period already set to %d hours", hours);
    return true;
  }

  // Write new ABC period
  uint16_t new_period = (uint16_t)hours;
  if (!_write_registers(HR_ABC_PERIOD, 1, &new_period)) {
    ESP_LOGE(TAG, "Failed to write ABC period");
    return false;
  }

  ESP_LOGI(TAG, "ABC period set to %d hours", hours);
  return true;
}

int Sunlight::get_abc_period() {
  if (!_read_registers(FUNC_READ_HOLDING_REGISTERS, HR_ABC_PERIOD, 1)) {
    ESP_LOGE(TAG, "Failed to read ABC period");
    return -1;
  }

  return (int)_values[0];
}

bool Sunlight::set_measurement_mode(MeasurementMode mode) {
  // Check if already in target mode
  if (_current_mode == mode) {
    ESP_LOGI(TAG, "Measurement mode already set to %s",
             (mode == MODE_SINGLE) ? "single" : "continuous");
    return false; // No change needed
  }

  // Write new mode
  uint16_t mode_value = static_cast<uint16_t>(mode);
  if (!_write_registers(HR_MEASUREMENT_MODE, 1, &mode_value)) {
    ESP_LOGE(TAG, "Failed to write measurement mode");
    return false;
  }

  // Update cached mode
  _current_mode = mode;

  ESP_LOGI(TAG, "Measurement mode changed to %s", (mode == MODE_SINGLE) ? "single" : "continuous");
  return true;
}

bool Sunlight::set_measurement_period(uint16_t seconds) {
  // Read current period
  if (!_read_registers(FUNC_READ_HOLDING_REGISTERS, HR_MEASUREMENT_PERIOD, 1)) {
    ESP_LOGE(TAG, "Failed to read measurement period");
    return false;
  }

  if (_values[0] == seconds) {
    ESP_LOGI(TAG, "Measurement period already set to %d seconds", seconds);
    return false; // No change needed
  }

  // Write new period
  if (!_write_registers(HR_MEASUREMENT_PERIOD, 1, &seconds)) {
    ESP_LOGE(TAG, "Failed to write measurement period");
    return false;
  }

  ESP_LOGI(TAG, "Measurement period set to %d seconds", seconds);
  ESP_LOGW(TAG, "Sensor restart required for changes to take effect");
  return true;
}

bool Sunlight::set_measurement_samples(uint16_t samples) {
  // Read current samples
  if (!_read_registers(FUNC_READ_HOLDING_REGISTERS, HR_MEASUREMENT_SAMPLES, 1)) {
    ESP_LOGE(TAG, "Failed to read measurement samples");
    return false;
  }

  if (_values[0] == samples) {
    ESP_LOGI(TAG, "Measurement samples already set to %d", samples);
    return false; // No change needed
  }

  // Write new samples
  if (!_write_registers(HR_MEASUREMENT_SAMPLES, 1, &samples)) {
    ESP_LOGE(TAG, "Failed to write measurement samples");
    return false;
  }

  ESP_LOGI(TAG, "Measurement samples set to %d", samples);
  ESP_LOGW(TAG, "Sensor restart required for changes to take effect");
  return true;
}

bool Sunlight::trigger_measurement() {
  // Trigger single measurement by writing to HR34
  uint16_t trigger = 0x0001;
  if (!_write_registers(HR_START_SINGLE_MEASUREMENT, 1, &trigger)) {
    ESP_LOGE(TAG, "Failed to trigger single measurement");
    return false;
  }

  ESP_LOGD(TAG, "Single measurement triggered");
  return true;
}

bool Sunlight::is_single_mode() {
  if (!_read_registers(FUNC_READ_HOLDING_REGISTERS, HR_MEASUREMENT_MODE, 1)) {
    ESP_LOGE(TAG, "Failed to read measurement mode");
    return false; // Assume continuous mode on error
  }

  return (_values[0] == MODE_SINGLE);
}

bool Sunlight::print_device_id() {
  char id[64];
  memset(id, 0, 64);

  // Vendor Name
  if (!_read_device_identification(0, id, sizeof(id))) {
    ESP_LOGE(TAG, "Failed to read vendor name");
    return false;
  }
  ESP_LOGI(TAG, "Vendor name: %s", id);

  // Product code
  memset(id, 0, 64);
  if (!_read_device_identification(1, id, sizeof(id))) {
    ESP_LOGE(TAG, "Failed to read product code");
    return false;
  }
  ESP_LOGI(TAG, "Product code: %s", id);

  // Major minor revision
  memset(id, 0, 64);
  if (!_read_device_identification(2, id, sizeof(id))) {
    ESP_LOGE(TAG, "Failed to read MajorMinorRevision");
    return false;
  }
  ESP_LOGI(TAG, "MajorMinorRevision: %s", id);

  return true;
}

// Private helper methods

bool Sunlight::_read_registers(uint8_t func, uint16_t reg, uint16_t num_reg) {
  // Build Modbus request
  _request[0] = MODBUS_ADDRESS;
  _request[1] = func;
  _request[2] = (reg >> 8) & 0xFF;
  _request[3] = reg & 0xFF;
  _request[4] = (num_reg >> 8) & 0xFF;
  _request[5] = num_reg & 0xFF;

  // Calculate CRC
  uint16_t crc = modbus_crc16(_request, 6);
  _request[6] = crc & 0xFF;
  _request[7] = (crc >> 8) & 0xFF;

  // Send request
  _serial.write(_request, 8);

  // Wait for response
  int expected_bytes = 5 + (num_reg * 2); // Address + Func + ByteCount + Data + CRC
  int num_bytes = _wait_for_response(expected_bytes);

  if (num_bytes < 0) {
    ESP_LOGE(TAG, "Timeout waiting for response");
    return false;
  }

  // Validate response
  if (!_validate_response(func, num_bytes)) {
    return false;
  }

  // Extract register values from response
  int data_offset = 3; // Skip address, function, byte count
  for (uint16_t i = 0; i < num_reg; i++) {
    _values[i] = ((uint16_t)_response[data_offset] << 8) | _response[data_offset + 1];
    data_offset += 2;
  }

  return true;
}

bool Sunlight::_write_registers(uint16_t reg, uint16_t num_reg, const uint16_t *values) {
  uint8_t num_bytes = num_reg * 2;

  // Build Modbus request
  _request[0] = MODBUS_ADDRESS;
  _request[1] = FUNC_WRITE_MULTIPLE_REGISTERS;
  _request[2] = (reg >> 8) & 0xFF;
  _request[3] = reg & 0xFF;
  _request[4] = (num_reg >> 8) & 0xFF;
  _request[5] = num_reg & 0xFF;
  _request[6] = num_bytes;

  // Add register values
  int offset = 7;
  for (uint16_t i = 0; i < num_reg; i++) {
    _request[offset++] = (values[i] >> 8) & 0xFF;
    _request[offset++] = values[i] & 0xFF;
  }

  // Calculate CRC
  uint16_t crc = modbus_crc16(_request, offset);
  _request[offset++] = crc & 0xFF;
  _request[offset++] = (crc >> 8) & 0xFF;

  // Send request
  _serial.write(_request, offset);

  // Wait for response (write response is 8 bytes)
  int response_bytes = _wait_for_response(8);
  if (response_bytes < 0) {
    ESP_LOGE(TAG, "Timeout waiting for write response");
    return false;
  }

  // Validate response
  if (!_validate_response(FUNC_WRITE_MULTIPLE_REGISTERS, response_bytes)) {
    return false;
  }

  return true;
}

bool Sunlight::_read_device_identification(uint8_t object_id, char *out, size_t out_len) {
  uint8_t request[7];

  // Build Modbus Device ID request
  request[0] = MODBUS_ADDRESS;
  request[1] = 0x2B; // Encapsulated Interface
  request[2] = 0x0E; // MEI type: Device Identification
  request[3] = 0x04; // Read Basic Device Identification
  request[4] = object_id;

  // CRC
  uint16_t crc = modbus_crc16(request, 5);
  request[5] = crc & 0xFF;
  request[6] = crc >> 8;

  // Send request
  _serial.write(request, 7);

  // Response is variable-length, wait for "enough" with longer timeout
  // Device ID responses can be slow, use 2000ms timeout
  int num_bytes = _wait_for_response(256, 2000);
  if (num_bytes < 5) {
    ESP_LOGE(TAG, "Invalid or no Device ID response (received %d bytes)", num_bytes);
    return false;
  }

  // // Log received bytes for debugging
  // ESP_LOGD(TAG, "Device ID response: %d bytes", num_bytes);
  // if (num_bytes > 0) {
  //   char hexStr[128];
  //   int offset = 0;
  //   for (int i = 0; i < num_bytes && i < 20; i++) {
  //     offset += snprintf(hexStr + offset, sizeof(hexStr) - offset, "%02X ",
  //     _response[i]);
  //   }
  //   if (num_bytes > 20) {
  //     snprintf(hexStr + offset, sizeof(hexStr) - offset, "...");
  //   }
  //   ESP_LOGD(TAG, "Raw data: %s", hexStr);
  // }

  // Check for Modbus exception response (5 bytes: Addr + ExceptionFunc + Code +
  // CRC)
  if (num_bytes == 5 && (_response[1] & 0x80)) {
    uint8_t exception_code = _response[2];
    ESP_LOGW(TAG, "Device ID not supported - Modbus exception: code %d", exception_code);
    return false;
  }

  // Check minimum length for valid Device ID response
  if (num_bytes < 9) {
    ESP_LOGE(TAG, "Device ID response too short (received %d bytes, expected >=9)", num_bytes);
    return false;
  }

  // Validate header
  if (_response[0] != MODBUS_ADDRESS || _response[1] != 0x2B || _response[2] != 0x0E) {
    ESP_LOGE(TAG, "Invalid Device ID header (addr=0x%02X, func=0x%02X, mei=0x%02X)", _response[0],
             _response[1], _response[2]);
    return false;
  }

  /*
   Response layout:
   [0] Addr
   [1] 0x2B
   [2] 0x0E
   [3] ReadCode
   [4] ConformityLevel
   [5] MoreFollows
   [6] NextObjectId
   [7] ObjectCount
   [8...] Objects
  */

  uint8_t obj_count = _response[7];
  int idx = 8;

  for (uint8_t i = 0; i < obj_count; i++) {
    if (idx + 2 > num_bytes)
      break;

    uint8_t id = _response[idx++];
    uint8_t len = _response[idx++];

    if (idx + len > num_bytes)
      break;

    if (id == object_id) {
      size_t copy_len = (len < out_len - 1) ? len : out_len - 1;
      memcpy(out, &_response[idx], copy_len);
      out[copy_len] = '\0';
      return true;
    }

    idx += len;
  }

  ESP_LOGW(TAG, "Device ID object %d not found", object_id);
  return false;
}

int Sunlight::_wait_for_response(int expected_bytes, uint32_t timeout_ms) {
  uint64_t start_time = RTOS::get_time_ms();
  int available_bytes = 0;
  uint64_t last_byte_time = 0;

  // Wait for first byte
  while (_serial.available() == 0) {
    uint64_t elapsed = RTOS::get_time_ms() - start_time;
    if (elapsed > timeout_ms) {
      return -1; // Timeout
    }
  }

  // Wait for all bytes with inter-packet detection
  last_byte_time = RTOS::get_time_ms();

  while (true) {
    int new_available = _serial.available();
    uint64_t current_time = RTOS::get_time_ms();

    if (new_available != available_bytes) {
      // New bytes arrived
      available_bytes = new_available;
      last_byte_time = current_time;
    } else if (current_time - last_byte_time >= INTER_PACKET_INTERVAL_MS) {
      // No new bytes for inter-packet interval, assume complete
      break;
    }

    // Check overall timeout
    if (current_time - start_time > timeout_ms) {
      ESP_LOGW(TAG, "Response timeout after %d bytes", available_bytes);
      break;
    }
  }

  // Read available bytes
  int bytes_to_read = (available_bytes < BUF_SIZE) ? available_bytes : BUF_SIZE;
  for (int i = 0; i < bytes_to_read; i++) {
    _response[i] = _serial.read();
  }

  return bytes_to_read;
}

bool Sunlight::_validate_response(uint8_t func, uint8_t num_bytes) {
  // Minimum response is 5 bytes (addr + func + data + CRC)
  if (num_bytes < 5) {
    ESP_LOGE(TAG, "Response too short: %d bytes", num_bytes);
    return false;
  }

  // Verify CRC
  uint16_t expected_crc = modbus_crc16(_response, num_bytes - 2);
  uint16_t received_crc = _response[num_bytes - 2] | (_response[num_bytes - 1] << 8);

  if (expected_crc != received_crc) {
    ESP_LOGE(TAG, "CRC mismatch: expected 0x%04X, got 0x%04X", expected_crc, received_crc);
    return false;
  }

  // Check for Modbus exception
  if (_response[1] == (func | 0x80)) {
    uint8_t exception_code = _response[2];
    ESP_LOGE(TAG, "Modbus exception: code %d", exception_code);
    return false;
  }

  // Verify function code
  if (_response[1] != func) {
    ESP_LOGE(TAG, "Function code mismatch: expected 0x%02X, got 0x%02X", func, _response[1]);
    return false;
  }

  return true;
}

int Sunlight::_get_error_status() {
  if (!_read_registers(FUNC_READ_INPUT_REGISTERS, IR_ERROR_STATUS, 1)) {
    return -1;
  }

  return (int)_values[0];
}

bool Sunlight::_clear_calibration_status() {
  uint16_t reset_value = CAL_STATUS_RESET;
  if (!_write_registers(HR_CALIBRATION_STATUS, 1, &reset_value)) {
    ESP_LOGE(TAG, "Failed to clear calibration status");
    return false;
  }

  return true;
}

int Sunlight::_get_calibration_status() {
  if (!_read_registers(FUNC_READ_HOLDING_REGISTERS, HR_CALIBRATION_STATUS, 1)) {
    return -1;
  }

  return (int)_values[0];
}

bool Sunlight::power_cycle() {
  if (_io_power == GPIO_NUM_MAX) {
    ESP_LOGW(TAG, "Sensor restart required for changes to take effect");
    return false;
  }

  gpio_set_level(_io_power, 0); // Turn off CO2 sensor
  RTOS::delay_ms(2000);
  gpio_set_level(_io_power, 1); // Turn on CO2 sensor
  RTOS::delay_ms(3000);         // stabilize

  // Wait for valid CO2 reading (non-zero value)
  ESP_LOGI(TAG, "Waiting for valid CO2 reading after restart...");
  int retry_count = 0;
  const int max_retries = 30; // Maximum 30 attempts (about 30 seconds)

  do {
    CO2Data data;
    read(data);

    retry_count++;
    ESP_LOGI(TAG, "CO2 reading attempt %d: %d ppm", retry_count, data.co2);

    if (data.is_valid()) {
      ESP_LOGI(TAG, "Valid CO2 reading obtained: %d ppm", data.co2);
      break;
    }

    RTOS::delay_ms(2400); // wait 1 second before next attempt
  } while (retry_count < max_retries);

  if (retry_count >= max_retries) {
    ESP_LOGW(TAG, "Maximum retry attempts reached, proceeding with "
                  "current reading");
    return false;
  }

  return true;
}
