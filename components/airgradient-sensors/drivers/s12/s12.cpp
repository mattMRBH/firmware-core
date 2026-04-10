/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/s12/s12.h"

#include "esp_log.h"

#include "rtos.h"

static constexpr const char *TAG = "S12";

S12::S12(i2c_master_bus_handle_t i2c_bus, uint8_t address, uint8_t co2_reg_hi)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _address(address), _co2_reg_hi(co2_reg_hi),
      _initialized(false) {}

bool S12::init() {
  if (_initialized) {
    ESP_LOGW(TAG, "init() called more than once, ignoring");
    return true;
  }

  // Probe I2C bus to verify device exists (with retry for boot timing).
  bool probed = false;
  for (int i = 0; i < INIT_PROBE_RETRIES; i++) {
    esp_err_t ret = i2c_master_probe(_i2c_bus, _address, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
      probed = true;
      break;
    }
    ESP_LOGW(TAG, "Probe attempt %d/%d failed: %s", i + 1, INIT_PROBE_RETRIES,
             esp_err_to_name(ret));
    RTOS::delay_ms(INIT_PROBE_DELAY_MS);
  }
  if (!probed) {
    ESP_LOGE(TAG, "S12 not found at address 0x%02X", _address);
    return false;
  }

  ESP_LOGI(TAG, "S12 found at address 0x%02X", _address);

  // Add device to I2C bus.
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = I2C_CLOCK_HZ,
      .scl_wait_us = 0,
      .flags = {},
  };

  esp_err_t ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add S12 device: %s", esp_err_to_name(ret));
    _dev_handle = nullptr;
    return false;
  }

  _initialized = true;
  ESP_LOGI(TAG, "S12 initialized (CO2 reg 0x%02X)", _co2_reg_hi);
  return true;
}

bool S12::read(CO2Data &out) {
  // Initialize to invalid sentinel before any early return.
  out.co2 = MeasuresInvalid::CO2;

  if (!_initialized || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Sensor not initialized");
    return false;
  }

  uint8_t data[CO2_REG_READ_LEN] = {0};
  if (!_read_register(_co2_reg_hi, data, sizeof(data))) {
    ESP_LOGE(TAG, "Failed to read CO2 register 0x%02X", _co2_reg_hi);
    return false;
  }

  // Big-endian 16-bit value directly in ppm.
  uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
  out.co2 = static_cast<int>(raw);

  ESP_LOGD(TAG, "CO2: %d ppm", out.co2);
  return true;
}

TempHumData S12::temp_hum_data() {
  return TempHumData{MeasuresInvalid::TEMPERATURE, MeasuresInvalid::HUMIDITY};
}

bool S12::_read_register(uint8_t reg, uint8_t *buf, size_t len) {
  if (_dev_handle == nullptr || buf == nullptr || len == 0) {
    return false;
  }

  esp_err_t last_err = ESP_FAIL;
  for (int i = 0; i < IO_RETRY_COUNT; i++) {
    last_err = i2c_master_transmit_receive(_dev_handle, &reg, 1, buf, len, I2C_TIMEOUT_MS);
    if (last_err == ESP_OK) {
      return true;
    }

    // Tickle the bus in case the target is waking up.
    (void)i2c_master_probe(_i2c_bus, _address, IO_PROBE_TICKLE_MS);
    RTOS::delay_ms(IO_RETRY_DELAY_MS);
  }

  ESP_LOGW(TAG, "read reg 0x%02X failed after %d retries: %s", reg, IO_RETRY_COUNT,
           esp_err_to_name(last_err));
  return false;
}
