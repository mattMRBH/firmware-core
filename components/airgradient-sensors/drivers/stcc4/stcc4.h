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

#ifndef STCC4_HPP
#define STCC4_HPP

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/co2_sensor.h"

/**
 * @brief Sensirion STCC4 CO2 sensor driver with integrated temperature and
 * humidity
 *
 * Communicates with sensor using I2C protocol.
 * Uses continuous measurement mode.
 * Provides CO2 concentration (ppm), temperature (C), and relative humidity (%).
 *
 * Raw data conversion:
 * - CO2: raw int16 value directly in ppm
 * - Temperature: -45 + 175 * (raw / 65535)
 * - Humidity: -6 + 125 * (raw / 65535)
 */
class STCC4 : public CO2Sensor {
public:
  /**
   * @brief I2C address
   */
  static constexpr uint8_t ADDRESS_DEFAULT = 0x64;

  /**
   * @brief Construct STCC4 sensor with I2C bus
   * @param i2c_bus I2C master bus handle
   * @param address I2C address (default 0x64)
   */
  explicit STCC4(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_DEFAULT);
  virtual ~STCC4() = default;

  // CO2Sensor interface implementation
  bool init() override;
  bool read(CO2Data &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;
  uint8_t _address;
  bool _measuring;

  // Cached last temp/hum reading from the most recent read()
  TempHumData _last_temp_hum;

  // I2C configuration
  static constexpr uint32_t I2C_CLOCK_HZ = 100000;
  static constexpr int I2C_TIMEOUT_MS = 500;

  // STCC4 I2C commands
  static constexpr uint16_t CMD_START_CONTINUOUS = 0x218B;
  static constexpr uint16_t CMD_READ_MEASUREMENT = 0xEC05;
  static constexpr uint16_t CMD_STOP_CONTINUOUS = 0x3F86;

  // Timing
  static constexpr int READ_DELAY_MS = 1;
  static constexpr int STOP_DELAY_MS = 1200;

  // Initialization retry parameters
  static constexpr int INIT_PROBE_RETRIES = 3;
  static constexpr int INIT_PROBE_DELAY_MS = 100;
  static constexpr int START_MEASUREMENT_RETRIES = 3;
  static constexpr int START_MEASUREMENT_RETRY_DELAY_MS = 100;

  // Data sizes (in data bytes, excluding CRC)
  // Read measurement returns: co2(2) + temp(2) + hum(2) + status(2) = 8 data bytes
  // With CRC: each 2 data bytes has 1 CRC byte = 12 I2C bytes total
  static constexpr uint16_t MEASUREMENT_I2C_SIZE = 12;

  /**
   * @brief Calculate CRC-8 for Sensirion I2C protocol
   * Polynomial: 0x31, Init: 0xFF
   */
  static uint8_t _calc_crc8(const uint8_t *data, uint8_t len);

  /**
   * @brief Send a 16-bit I2C command (no data payload)
   * @return true if successful
   */
  bool _write_command(uint16_t cmd);

  /**
   * @brief Read data from I2C with CRC verification and in-place extraction
   * Strips CRC bytes and returns only data bytes
   * @param buffer Buffer for raw I2C data (must hold i2c_len bytes)
   * @param i2c_len Number of raw I2C bytes to read (with CRC, multiple of 3)
   * @param data_out Buffer for extracted data bytes (without CRC)
   * @param data_len Expected data length (i2c_len * 2 / 3)
   * @return true if successful and all CRCs valid
   */
  bool _read_data(uint8_t *buffer, uint16_t i2c_len, uint8_t *data_out, uint16_t data_len);
};

#endif // STCC4_HPP
