/**
 * AirGradient Go — LIS2DH12 3-axis accelerometer driver
 *
 * Minimal I2C driver for the ST LIS2DH12 (Go-only). Provides a WHO_AM_I
 * identity check, init (100 Hz, normal mode, ±2 g, high-resolution), and a
 * raw 3-axis read in milli-g.
 *
 * Poll-only: the INT1 line (net ACC_INT → ESP32-C5 IO3) is wired on the
 * board but not used by this driver; the chip runs in continuous-read mode.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "accel_sensor.h"

#include <driver/i2c_master.h>

#include <cstddef>
#include <cstdint>

class LIS2DH12 : public AccelSensor {
public:
  struct Config {
    uint8_t address = 0x18;         ///< 7-bit address (SA0 = GND)
    uint32_t scl_speed_hz = 400000; ///< 400 kHz fast-mode
    int timeout_ms = 50;
  };

  /// Expected response from the WHO_AM_I register (datasheet §8.7).
  static constexpr uint8_t WHO_AM_I_EXPECTED = 0x33;

  LIS2DH12(i2c_master_bus_handle_t bus, const Config &config);
  ~LIS2DH12() override;

  LIS2DH12(const LIS2DH12 &) = delete;
  LIS2DH12 &operator=(const LIS2DH12 &) = delete;

  /// Probe + configure for 100 Hz continuous reads at ±2 g, high-resolution
  /// mode (12-bit, 1 mg/LSB). Verifies WHO_AM_I matches 0x33. Returns false
  /// on any I2C error or wrong WHO_AM_I.
  bool init() override;

  /// Read the WHO_AM_I register (0x0F). Returns the raw byte or 0 on error.
  uint8_t who_am_i() override;

  uint8_t expected_who_am_i() const override { return WHO_AM_I_EXPECTED; }

  /// Read X/Y/Z and convert to milli-g. Returns false on I2C error.
  /// Values are signed; gravity registers as ~+1000 mg on the axis pointing up.
  bool read(AccelReading &out) override;

private:
  // Register addresses (LIS2DH12 datasheet §6)
  static constexpr uint8_t REG_WHO_AM_I = 0x0F;
  static constexpr uint8_t REG_CTRL_REG1 = 0x20;
  static constexpr uint8_t REG_CTRL_REG4 = 0x23;
  static constexpr uint8_t REG_OUT_X_L = 0x28;
  // Auto-increment flag (bit 7) set when reading a register block.
  static constexpr uint8_t AUTO_INCR = 0x80;

  // CTRL_REG1 = 0x57 → ODR=0101 (100 Hz), LPen=0 (normal), Z/Y/X enabled.
  static constexpr uint8_t CTRL_REG1_CONFIG = 0x57;
  // CTRL_REG4 = 0x88 → BDU=1 (block update), FS=00 (±2 g), HR=1 (high-res).
  static constexpr uint8_t CTRL_REG4_CONFIG = 0x88;

  // First conversion after enabling needs one ODR period (10 ms @ 100 Hz);
  // wait a little longer so the first read is meaningful.
  static constexpr int STARTUP_DELAY_MS = 20;

  Config _config;
  i2c_master_bus_handle_t _bus = nullptr;
  i2c_master_dev_handle_t _dev = nullptr;

  bool _write_reg(uint8_t reg, uint8_t value);
  bool _read_reg(uint8_t reg, uint8_t &out);
  bool _read_block(uint8_t reg, uint8_t *buf, size_t len);
};
