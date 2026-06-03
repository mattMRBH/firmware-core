/**
 * AirGradient Go -- LP5036 LED driver declaration
 *
 * Concrete LedDriver implementation for the LP5036 36-channel I2C
 * constant-current LED controller.  Production wiring includes this
 * header; LedService does not.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_led_hal.h"

#include <cstddef>
#include <cstdint>

#ifndef TEST_HOST
#include <driver/i2c_master.h>
using LedI2cBusHandle = i2c_master_bus_handle_t;
using LedI2cDevHandle = i2c_master_dev_handle_t;
#else
using LedI2cBusHandle = void *;
using LedI2cDevHandle = void *;
#endif

class LP5036 final : public LedDriver {
public:
  struct Config {
    uint8_t address = 0x33;
    uint32_t scl_speed_hz = 400000;
    int timeout_ms = 100;
  };

  LP5036(LedI2cBusHandle bus, const Config &config);
  ~LP5036() override;

  LP5036(const LP5036 &) = delete;
  LP5036 &operator=(const LP5036 &) = delete;

  bool init() override;
  bool set_channel(uint8_t channel, uint8_t pwm) override;
  bool set_rgb(uint8_t b_channel, uint8_t r, uint8_t g, uint8_t b) override;

private:
  Config _config;
  LedI2cBusHandle _bus = nullptr;
  LedI2cDevHandle _dev = nullptr;

  bool _write_reg(uint8_t reg, uint8_t value);
  bool _write_block(uint8_t reg, const uint8_t *data, size_t len);
};
