/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "types/gps_types.h"

// Execution context: task context only
// ISR-safe: no
// Thread-safe: no
// Blocking: no — read() drains whatever bytes are currently buffered
// Allocates: no
// Ownership: implementations must not retain pointers across calls
class GpsSensor {
public:
  virtual ~GpsSensor() = default;

  /// Initialize the GPS sensor with the given baud rate.
  virtual bool begin(int baud_rate) = 0;

  /// Shut down the GPS sensor.
  virtual void end() = 0;

  /// Process all available serial data and update internal state.
  /// Non-blocking: only consumes bytes currently in the serial buffer.
  /// Returns true if at least one complete NMEA sentence was received.
  virtual bool read() = 0;

  /// Get the latest GPS data snapshot.
  virtual GpsData get_data() const = 0;

  /// Check if the current fix is valid (2D or 3D).
  virtual bool has_valid_fix() const = 0;
};
