/*
 * Copyright (c) 2025, Sensirion AG
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Sensirion AG nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "drivers/stcc4/stcc4.h"

#include "esp_log.h"

#include "rtos.h"

static constexpr const char *TAG = "STCC4";

STCC4::STCC4(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _address(address), _measuring(false),
      _last_temp_hum{MeasuresInvalid::TEMPERATURE, MeasuresInvalid::HUMIDITY} {}

bool STCC4::init() {
  // Probe I2C bus to verify device exists (with retry for boot timing)
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
    ESP_LOGE(TAG, "STCC4 not found at address 0x%02X", _address);
    return false;
  }

  ESP_LOGI(TAG, "STCC4 found at address 0x%02X", _address);

  // Add device to I2C bus
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = I2C_CLOCK_HZ,
      .scl_wait_us = 0,
      .flags = {},
  };

  esp_err_t ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add STCC4 device: %s", esp_err_to_name(ret));
    return false;
  }

  uint16_t data_ready_status = 0;
  if (_read_data_ready_status(data_ready_status)) {
    _measuring = true;
    ESP_LOGI(TAG, "STCC4 already in continuous measurement mode");
    return true;
  }

  // Start continuous measurement mode (with retry for transient I2C failures)
  bool started = false;
  for (int i = 0; i < START_MEASUREMENT_RETRIES; i++) {
    if (_write_command(CMD_START_CONTINUOUS)) {
      started = true;
      break;
    }
    ESP_LOGW(TAG, "Start measurement attempt %d/%d failed", i + 1, START_MEASUREMENT_RETRIES);
    RTOS::delay_ms(START_MEASUREMENT_RETRY_DELAY_MS);
  }
  if (!started) {
    ESP_LOGE(TAG, "Failed to start continuous measurement after %d attempts",
             START_MEASUREMENT_RETRIES);
    return false;
  }

  _measuring = true;
  ESP_LOGI(TAG, "STCC4 initialized, continuous measurement started");
  return true;
}

bool STCC4::read(CO2Data &out) {
  // Initialize to invalid sentinel
  out.co2 = MeasuresInvalid::CO2;

  if (!_measuring || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Sensor not initialized or not measuring");
    return false;
  }

  // Read measurement: 3 words x 3 bytes (data[2] + CRC[1]) = 9 bytes
  // Data: co2_raw(2) + temp_raw(2) + hum_raw(2) = 6 data bytes
  uint8_t i2c_buf[MEASUREMENT_I2C_SIZE];
  uint8_t data_buf[MEASUREMENT_DATA_SIZE];

  bool read_ok = false;
  for (int i = 0; i < READ_MEASUREMENT_RETRIES; i++) {
    uint16_t data_ready_status = 0;
    if (!_read_data_ready_status(data_ready_status)) {
      ESP_LOGW(TAG, "Data-ready status attempt %d/%d failed", i + 1, READ_MEASUREMENT_RETRIES);
    } else if ((data_ready_status & DATA_READY_STATUS_MASK) == 0U) {
      ESP_LOGW(TAG, "Measurement not ready on attempt %d/%d", i + 1, READ_MEASUREMENT_RETRIES);
    } else if (!_write_command(CMD_READ_MEASUREMENT)) {
      ESP_LOGW(TAG, "Read measurement command attempt %d/%d failed", i + 1,
               READ_MEASUREMENT_RETRIES);
    } else {
      RTOS::delay_ms(READ_DELAY_MS);

      if (_read_data(i2c_buf, MEASUREMENT_I2C_SIZE, data_buf, MEASUREMENT_DATA_SIZE)) {
        read_ok = true;
        break;
      }

      ESP_LOGW(TAG, "Read measurement data attempt %d/%d failed", i + 1, READ_MEASUREMENT_RETRIES);
    }

    if (i + 1 < READ_MEASUREMENT_RETRIES) {
      RTOS::delay_ms(READ_MEASUREMENT_RETRY_DELAY_MS);
    }
  }

  if (!read_ok) {
    ESP_LOGE(TAG, "Failed to read measurement after %d attempts", READ_MEASUREMENT_RETRIES);
    return false;
  }

  // Parse CO2 (signed int16, directly in ppm)
  int16_t co2_raw = static_cast<int16_t>((static_cast<uint16_t>(data_buf[0]) << 8) | data_buf[1]);

  // Parse temperature raw (unsigned int16)
  uint16_t temp_raw = (static_cast<uint16_t>(data_buf[2]) << 8) | data_buf[3];

  // Parse humidity raw (unsigned int16)
  uint16_t hum_raw = (static_cast<uint16_t>(data_buf[4]) << 8) | data_buf[5];

  // Convert CO2
  out.co2 = static_cast<int>(co2_raw);

  // Convert temperature: -45 + 175 * (raw / 65535)
  _last_temp_hum.temperature = -45.0f + 175.0f * (static_cast<float>(temp_raw) / 65535.0f);

  // Convert humidity: -6 + 125 * (raw / 65535), clamped to [0, 100]
  float hum = -6.0f + 125.0f * (static_cast<float>(hum_raw) / 65535.0f);
  if (hum < 0.0f) {
    hum = 0.0f;
  }
  if (hum > 100.0f) {
    hum = 100.0f;
  }
  _last_temp_hum.humidity = hum;

  ESP_LOGD(TAG, "CO2: %d ppm, Temp: %.2f C, Hum: %.2f%%", out.co2, _last_temp_hum.temperature,
           _last_temp_hum.humidity);

  return true;
}

bool STCC4::supports_temp_hum() const { return true; }

TempHumData STCC4::temp_hum_data() { return _last_temp_hum; }

uint8_t STCC4::_calc_crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x31;
      } else {
        crc = (crc << 1);
      }
    }
  }
  return crc;
}

bool STCC4::_write_command(uint16_t cmd) {
  if (_dev_handle == nullptr) {
    return false;
  }

  uint8_t buf[2];
  buf[0] = (cmd >> 8) & 0xFF;
  buf[1] = cmd & 0xFF;

  esp_err_t ret = i2c_master_transmit(_dev_handle, buf, 2, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "I2C write command 0x%04X failed: %s", cmd, esp_err_to_name(ret));
    return false;
  }
  return true;
}

bool STCC4::_read_data_ready_status(uint16_t &status_out) {
  uint8_t i2c_buf[DATA_READY_STATUS_I2C_SIZE];
  uint8_t data_buf[DATA_READY_STATUS_DATA_SIZE];

  if (!_write_command(CMD_GET_DATA_READY_STATUS)) {
    return false;
  }

  RTOS::delay_ms(READ_DELAY_MS);

  if (!_read_data(i2c_buf, DATA_READY_STATUS_I2C_SIZE, data_buf, DATA_READY_STATUS_DATA_SIZE)) {
    return false;
  }

  status_out = (static_cast<uint16_t>(data_buf[0]) << 8) | data_buf[1];
  return true;
}

bool STCC4::_read_data(uint8_t *buffer, uint16_t i2c_len, uint8_t *data_out, uint16_t data_len) {
  if (_dev_handle == nullptr) {
    return false;
  }

  esp_err_t ret = i2c_master_receive(_dev_handle, buffer, i2c_len, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "I2C read failed: %s", esp_err_to_name(ret));
    return false;
  }

  // Verify CRC for every 3-byte word and extract data bytes
  uint16_t data_idx = 0;
  for (uint16_t i = 0; i + 2 < i2c_len; i += 3) {
    uint8_t expected_crc = _calc_crc8(&buffer[i], 2);
    if (buffer[i + 2] != expected_crc) {
      ESP_LOGW(TAG, "CRC mismatch at offset %d: got 0x%02x, expected 0x%02x", i, buffer[i + 2],
               expected_crc);
      return false;
    }
    if (data_idx + 1 < data_len) {
      data_out[data_idx++] = buffer[i];
      data_out[data_idx++] = buffer[i + 1];
    }
  }

  return data_idx == data_len;
}
