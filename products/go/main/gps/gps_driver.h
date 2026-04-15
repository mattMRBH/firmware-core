/**
 * AirGradient Go — GPS Driver
 *
 * Concrete NMEA GPS driver. Wraps an AirgradientSerial reference for all
 * serial I/O and parses GGA, RMC, and GSA sentences via libnmea-esp32.
 *
 * No virtual methods, no abstract base class. Testable by injecting a
 * StubSerial (byte-queue AirgradientSerial subclass) at construction.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "airgradient_serial.h"
#include "gps/gps_types.h"
#include "gpgga.h"
#include "gpgsa.h"
#include "gprmc.h"
#include "nmea.h"

class GpsDriver {
public:
  /// TAU1113 hardware default baud rate (factory setting, restored on every
  /// power-on reset).
  static constexpr int MODULE_DEFAULT_BAUD = 9600;

  explicit GpsDriver(AirgradientSerial &serial);
  ~GpsDriver();

  /// Initialise the serial link to the GPS module.
  ///
  /// If @p baud_rate differs from MODULE_DEFAULT_BAUD the driver first opens
  /// the UART at 9600 bps, sends the TAU1113 binary command to switch the
  /// module to 115200 bps, tears down the link and re-opens at the target
  /// rate.  If the module was already running at 115200 (e.g. it stayed
  /// powered during deep sleep) the 9600-baud command bytes are received as
  /// garbage and silently ignored, so the sequence is safe in both cases.
  bool begin(int baud_rate);
  void end();

  /// Process all available serial data and update internal state.
  /// Non-blocking: only consumes bytes currently in the serial buffer.
  /// Returns true if at least one complete NMEA sentence was received.
  bool read();

  /// Get the latest GPS data snapshot.
  GpsData get_data() const;

  /// Check if the current fix is valid (2D or 3D).
  bool has_valid_fix() const;

private:
  AirgradientSerial &_serial;

  // NMEA sentence accumulation buffer (NMEA 0183 max sentence is 82 chars;
  // 256 bytes provides ample headroom and allows detecting oversized input).
  static constexpr size_t BUFFER_SIZE = 256;
  char _buffer[BUFFER_SIZE];
  size_t _buffer_pos = 0;

  // Current parsed GPS state — initialized to invalid sentinels.
  GpsData _data;

  // Accumulate one byte into the buffer. Returns true when a complete
  // sentence (terminated by \r\n) has been received and dispatched.
  bool _process_byte(char byte);

  // Parse a complete NMEA sentence and update _data. The buffer contents
  // are modified in-place by libnmea (nmea_parse uses _crop_sentence).
  void _handle_sentence(char *sentence, size_t length);

  // Per-sentence-type state updaters.
  void _process_gga(const nmea_gpgga_s *gga);
  void _process_rmc(const nmea_gprmc_s *rmc);
  void _process_gsa(const nmea_gpgsa_s *gsa);

  // Convert a libnmea nmea_position (degrees + decimal minutes + cardinal)
  // to signed decimal degrees. Negative for South / West.
  static double _to_decimal_degrees(const nmea_position &pos);

  // Send the TAU1113 binary command to switch the module UART to 115200 bps.
  void _send_baud_115200_command();
};
