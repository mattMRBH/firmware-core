/**
 * AirGradient Go — Accelerometer HAL
 *
 * Abstract interface for a 3-axis accelerometer. The Go board carries an
 * ST LIS2DH12; the interface exists so the orchestrator's hardware-test
 * flow and host tests depend on the abstraction, not the concrete driver.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

/// One 3-axis sample in milli-g. Gravity registers as ~+1000 mg on the
/// axis pointing up.
struct AccelReading {
  int16_t x_mg = 0;
  int16_t y_mg = 0;
  int16_t z_mg = 0;
};

/// Abstract 3-axis accelerometer.
///
/// ISR-safe: no
/// Thread-safe: no
/// Blocking: init() may block during device probe and configuration
/// Allocates: no (after init())
class AccelSensor {
public:
  virtual ~AccelSensor() = default;

  /// Probe + configure the device and verify its identity. Must be called
  /// before read(). Returns false if the device is absent or misconfigured.
  virtual bool init() = 0;

  /// Read the identity register. Returns the raw byte, or 0 on I2C error.
  virtual uint8_t who_am_i() = 0;

  /// Expected identity byte for this device, so an external identity check
  /// (e.g. the hardware-test screen) needs no knowledge of the concrete driver.
  virtual uint8_t expected_who_am_i() const = 0;

  /// Read one 3-axis sample in milli-g. Returns false on I2C error.
  virtual bool read(AccelReading &out) = 0;
};
