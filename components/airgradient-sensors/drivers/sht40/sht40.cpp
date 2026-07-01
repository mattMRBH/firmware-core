/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/sht40/sht40.h"

#include <cstring>

#include "esp_log.h"

#include "rtos.h"

static constexpr const char *TAG = "SHT40";

SHT40::SHT40(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _address(address), _accuracy(Accuracy::HIGH) {}

bool SHT40::init() {
  // Power-up delay before first I2C transaction
  RTOS::delay_ms(POWERUP_DELAY_MS);

  // Probe I2C bus to verify device exists
  esp_err_t ret = i2c_master_probe(_i2c_bus, _address, 1000);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to probe SHT40 at address 0x%02X: %s", _address, esp_err_to_name(ret));
    return false;
  }

  ESP_LOGI(TAG, "SHT40 found at address 0x%02X", _address);

  // Add device to I2C bus
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = 100000, // 100kHz for SHT40
      .scl_wait_us = 0,
      .flags = {},
  };

  ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add SHT40 device: %s", esp_err_to_name(ret));
    return false;
  }

  // Perform soft reset to ensure sensor is in known state
  if (!_reset()) {
    ESP_LOGE(TAG, "Failed to reset SHT40");
    return false;
  }

  // Application start delay
  RTOS::delay_ms(APPSTART_DELAY_MS);

  ESP_LOGI(TAG, "SHT40 initialized successfully");
  return true;
}

bool SHT40::read(TempHumData &out) {
  // Initialize to invalid sentinels
  out.temperature = MeasuresInvalid::TEMPERATURE;
  out.humidity = MeasuresInvalid::HUMIDITY;

  if (_dev_handle == nullptr) {
    ESP_LOGE(TAG, "Device not initialized");
    return false;
  }

  uint8_t cmd = _getMeasurementCommand();
  uint8_t duration = _getMeasurementDuration();
  uint8_t data[DATA_SIZE];

  // Send measurement command
  esp_err_t ret = i2c_master_transmit(_dev_handle, &cmd, 1, 1000);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send measurement command: %s", esp_err_to_name(ret));
    return false;
  }

  // Allow bus to settle after command transmission
  RTOS::delay_ms(10);

  // Wait for measurement to complete
  RTOS::delay_ms(duration);

  // Read measurement data with retry logic (sensor may still be busy)
  uint8_t retry_count = 0;
  do {
    ret = i2c_master_receive(_dev_handle, data, DATA_SIZE, 1000);
    if (ret == ESP_OK) {
      break; // Success, exit retry loop
    }
    // Delay before next retry attempt
    RTOS::delay_ms(RETRY_DELAY_MS);
  } while (++retry_count <= RETRY_MAX);

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read measurement data after %d retries: %s", retry_count,
             esp_err_to_name(ret));
    return false;
  }

  // Validate CRC for temperature (bytes 0-1, CRC at byte 2)
  if (_calculateCrc8(&data[0], 2) != data[2]) {
    ESP_LOGW(TAG, "Temperature CRC mismatch");
    return false;
  }

  // Validate CRC for humidity (bytes 3-4, CRC at byte 5)
  if (_calculateCrc8(&data[3], 2) != data[5]) {
    ESP_LOGW(TAG, "Humidity CRC mismatch");
    return false;
  }

  // Convert temperature
  uint16_t temp_raw = (data[0] << 8) | data[1];
  out.temperature = -45.0f + 175.0f * (temp_raw / 65535.0f);

  // Convert humidity
  uint16_t hum_raw = (data[3] << 8) | data[4];
  out.humidity = -6.0f + 125.0f * (hum_raw / 65535.0f);

  ESP_LOGD(TAG, "Temperature: %.2f°C, Humidity: %.2f%%", out.temperature, out.humidity);

  return true;
}

bool SHT40::set_accuracy(Accuracy accuracy) {
  _accuracy = accuracy;
  ESP_LOGI(TAG, "Accuracy set to %s",
           accuracy == Accuracy::HIGH     ? "HIGH"
           : accuracy == Accuracy::MEDIUM ? "MEDIUM"
                                          : "LOW");
  return true;
}

bool SHT40::_reset() {
  if (_dev_handle == nullptr) {
    ESP_LOGE(TAG, "Device not initialized");
    return false;
  }

  // Send soft reset command
  uint8_t cmd = CMD_RESET;
  esp_err_t ret = i2c_master_transmit(_dev_handle, &cmd, 1, 1000);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send reset command: %s", esp_err_to_name(ret));
    return false;
  }

  // Wait for reset to complete (per SHT4x datasheet)
  RTOS::delay_ms(RESET_DELAY_MS);

  // Optionally read serial number for verification and logging
  uint8_t serial_cmd = CMD_SERIAL;
  uint8_t serial_data[SERIAL_SIZE];

  ret = i2c_master_transmit(_dev_handle, &serial_cmd, 1, 1000);
  if (ret != ESP_OK) {
    ESP_LOGD(TAG, "Failed to request serial number (non-critical): %s", esp_err_to_name(ret));
    return true; // Non-critical, reset was successful
  }

  // Delay between transmit and receive
  RTOS::delay_ms(TX_RX_DELAY_MS);

  ret = i2c_master_receive(_dev_handle, serial_data, SERIAL_SIZE, 1000);
  if (ret == ESP_OK) {
    // Extract serial number (bytes 0-1 and 3-4, with CRCs at 2 and 5)
    uint32_t serial_number = ((uint32_t)serial_data[0] << 24) | ((uint32_t)serial_data[1] << 16) |
                             ((uint32_t)serial_data[3] << 8) | serial_data[4];
    ESP_LOGI(TAG, "SHT40 serial number: 0x%08lX", serial_number);
  } else {
    ESP_LOGD(TAG, "Failed to read serial number (non-critical): %s", esp_err_to_name(ret));
  }

  ESP_LOGI(TAG, "SHT40 reset successful");
  return true;
}

uint8_t SHT40::_calculateCrc8(const uint8_t *data, uint8_t len) const {
  // CRC-8 polynomial: 0x31 (x^8 + x^5 + x^4 + 1)
  // Initialization: 0xFF
  uint8_t crc = 0xFF;

  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 8; bit > 0; --bit) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x31;
      } else {
        crc = (crc << 1);
      }
    }
  }

  return crc;
}

uint8_t SHT40::_getMeasurementCommand() const {
  switch (_accuracy) {
  case Accuracy::HIGH:
    return CMD_MEASURE_HIGH;
  case Accuracy::MEDIUM:
    return CMD_MEASURE_MEDIUM;
  case Accuracy::LOW:
    return CMD_MEASURE_LOW;
  default:
    return CMD_MEASURE_HIGH;
  }
}

uint8_t SHT40::_getMeasurementDuration() const {
  switch (_accuracy) {
  case Accuracy::HIGH:
    return DURATION_HIGH_MS;
  case Accuracy::MEDIUM:
    return DURATION_MEDIUM_MS;
  case Accuracy::LOW:
    return DURATION_LOW_MS;
  default:
    return DURATION_HIGH_MS;
  }
}
