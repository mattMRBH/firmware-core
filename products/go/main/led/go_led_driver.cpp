/**
 * AirGradient Go -- LP5036 LED driver implementation
 *
 * I2C driver for the LP5036 36-channel constant-current LED controller.
 * Contains all ESP-IDF I2C dependencies; not linked into host tests.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_led_driver.h"

#include "ag_log.h"

#include <cstring>

static constexpr const char *TAG = "LP5036";

// ---------------------------------------------------------------------------
// LP5036 register map (only what we use)
// ---------------------------------------------------------------------------

static constexpr uint8_t REG_DEVICE_CONFIG0 = 0x00;
static constexpr uint8_t REG_DEVICE_CONFIG1 = 0x01;
static constexpr uint8_t REG_OUT0_COLOR = 0x14; // OUTn = REG_OUT0_COLOR + n

static constexpr uint8_t DEV_CONFIG0_CHIP_EN = 0x40;
// Bit 3 = AUTO_INCR_EN, Bit 5 = PWM_DITHERING_EN
static constexpr uint8_t DEV_CONFIG1_DEFAULT = 0b00111000;

// ===========================================================================
// Construction / Destruction
// ===========================================================================

LP5036::LP5036(LedI2cBusHandle bus, const Config &config) : _config(config), _bus(bus) {}

LP5036::~LP5036() {
#ifndef TEST_HOST
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
#endif
}

// ===========================================================================
// Public interface
// ===========================================================================

bool LP5036::init() {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = _config.address,
        .scl_speed_hz = _config.scl_speed_hz,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK) {
      AG_LOGE(TAG, "bus_add_device failed at addr 0x%02X", _config.address);
      return false;
    }
  }
#endif

  // Enable chip
  if (!_write_reg(REG_DEVICE_CONFIG0, DEV_CONFIG0_CHIP_EN)) {
    AG_LOGE(TAG, "enable failed (no ACK at 0x%02X?)", _config.address);
    return false;
  }
  if (!_write_reg(REG_DEVICE_CONFIG1, DEV_CONFIG1_DEFAULT)) {
    return false;
  }
  // Zero all 36 output channels -- boots dark
  uint8_t zeros[NUM_CHANNELS] = {};
  if (!_write_block(REG_OUT0_COLOR, zeros, NUM_CHANNELS)) {
    return false;
  }
  AG_LOGI(TAG, "initialised at I2C 0x%02X", _config.address);
  return true;
}

bool LP5036::set_channel(uint8_t channel, uint8_t pwm) {
  if (channel >= NUM_CHANNELS) {
    return false;
  }
  return _write_reg(REG_OUT0_COLOR + channel, pwm);
}

bool LP5036::set_rgb(uint8_t b_channel, uint8_t r, uint8_t g, uint8_t b) {
  if (b_channel + 2 >= NUM_CHANNELS) {
    return false;
  }
  // v0.3 mapping per OUT register order: B, G, R contiguous.
  return _write_reg(REG_OUT0_COLOR + b_channel, b) &&
         _write_reg(REG_OUT0_COLOR + b_channel + 1, g) &&
         _write_reg(REG_OUT0_COLOR + b_channel + 2, r);
}

// ===========================================================================
// Private I2C helpers
// ===========================================================================

bool LP5036::_write_reg(uint8_t reg, uint8_t value) {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {reg, value};
  return i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  (void)value;
  return true;
#endif
}

bool LP5036::_write_block(uint8_t reg, const uint8_t *data, size_t len) {
#ifndef TEST_HOST
  if (_dev == nullptr || len == 0 || len > 64) {
    return false;
  }
  static constexpr size_t MAX_BLOCK_LEN = 64;
  uint8_t buf[1 + MAX_BLOCK_LEN];
  buf[0] = reg;
  std::memcpy(buf + 1, data, len);
  return i2c_master_transmit(_dev, buf, len + 1, _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  (void)data;
  (void)len;
  return true;
#endif
}
