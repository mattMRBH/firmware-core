/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstddef>

#if __has_include("airgradient_serial.h")
#include "airgradient_serial.h"
#else
#include "../../airgradient-serial/include/airgradient_serial.h"
#endif

#include "hal/gps_sensor.h"
#include "nmea.h"
#include "gpgga.h"
#include "gprmc.h"
#include "gpgsa.h"

class TestableNmeaGps;

class NmeaGps : public GpsSensor {
  friend class TestableNmeaGps;

public:
  explicit NmeaGps(AirgradientSerial &serial);
  ~NmeaGps() override;

  bool begin(int baud_rate) override;
  void end() override;
  bool read() override;
  GpsData get_data() const override;
  bool has_valid_fix() const override;

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
};
