/**
 * AirGradient Go — GPS Driver implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "gps/gps_driver.h"

#include <cstdlib>

static constexpr const char *TAG = "GpsDriver";

GpsDriver::GpsDriver(AirgradientSerial &serial) : _serial(serial), _buffer_pos(0) {}

GpsDriver::~GpsDriver() {}

bool GpsDriver::begin(int baud_rate) { return _serial.begin(baud_rate); }

void GpsDriver::end() { _serial.end(); }

bool GpsDriver::read() {
  bool got_sentence = false;
  const int available = _serial.available();
  for (int i = 0; i < available; i++) {
    const int byte_val = _serial.read();
    if (byte_val < 0) {
      break;
    }
    if (_process_byte(static_cast<char>(byte_val))) {
      got_sentence = true;
    }
  }
  return got_sentence;
}

GpsData GpsDriver::get_data() const { return _data; }

bool GpsDriver::has_valid_fix() const { return _data.fix.fix_type != GpsFixType::NoFix; }

bool GpsDriver::_process_byte(char byte) {
  if (byte == '$') {
    // Start of a new sentence: reset the buffer and store the '$'.
    _buffer[0] = '$';
    _buffer_pos = 1;
    return false;
  }

  // Ignore bytes that arrive before the first '$'.
  if (_buffer_pos == 0) {
    return false;
  }

  // Buffer-overflow guard: discard this sentence and reset.
  if (_buffer_pos >= BUFFER_SIZE - 1) {
    _buffer_pos = 0;
    return false;
  }

  _buffer[_buffer_pos++] = byte;

  // Detect NMEA 0183 sentence terminator: \r\n
  if (byte == '\n' && _buffer_pos >= 2 && _buffer[_buffer_pos - 2] == '\r') {
    _handle_sentence(_buffer, _buffer_pos);
    _buffer_pos = 0;
    return true;
  }

  return false;
}

void GpsDriver::_handle_sentence(char *sentence, size_t length) {
  // nmea_parse validates the sentence (including checksum) and returns an
  // allocated struct, or NULL on any error. The buffer is modified in-place.
  nmea_s *parsed = nmea_parse(sentence, length, 1);
  if (parsed == nullptr) {
    return;
  }

  switch (parsed->type) {
  case NMEA_GPGGA:
    _process_gga(reinterpret_cast<const nmea_gpgga_s *>(parsed));
    break;
  case NMEA_GPRMC:
    _process_rmc(reinterpret_cast<const nmea_gprmc_s *>(parsed));
    break;
  case NMEA_GPGSA:
    _process_gsa(reinterpret_cast<const nmea_gpgsa_s *>(parsed));
    break;
  default:
    break;
  }

  nmea_free(parsed);
}

void GpsDriver::_process_gga(const nmea_gpgga_s *gga) {
  // Update satellite count unconditionally — it is informational even when
  // there is no fix.
  _data.fix.satellite_count = gga->n_satellites;

  if (gga->position_fix > 0) {
    _data.position.latitude = _to_decimal_degrees(gga->latitude);
    _data.position.longitude = _to_decimal_degrees(gga->longitude);
    _data.altitude_m = static_cast<float>(gga->altitude);
    // GGA does not distinguish 2D vs 3D — use Fix2D as a conservative base.
    // GSA, which typically follows GGA in the NMEA output stream, will refine
    // the fix type to Fix3D if applicable.
    _data.fix.fix_type = GpsFixType::Fix2D;
  } else {
    _data.fix.fix_type = GpsFixType::NoFix;
  }
}

void GpsDriver::_process_rmc(const nmea_gprmc_s *rmc) {
  // Update position from RMC only when GGA has not already provided a valid
  // position (avoids overwriting higher-quality GGA data).
  if (!is_position_valid(_data.position) && rmc->valid) {
    _data.position.latitude = _to_decimal_degrees(rmc->latitude);
    _data.position.longitude = _to_decimal_degrees(rmc->longitude);
  }

  // Always update the timestamp — RMC is the authoritative date/time source.
  _data.timestamp.year = rmc->date_time.tm_year + 1900;
  _data.timestamp.month = rmc->date_time.tm_mon + 1;
  _data.timestamp.day = rmc->date_time.tm_mday;
  _data.timestamp.hour = rmc->date_time.tm_hour;
  _data.timestamp.minute = rmc->date_time.tm_min;
  _data.timestamp.second = rmc->date_time.tm_sec;
  _data.timestamp.valid = rmc->valid;
}

void GpsDriver::_process_gsa(const nmea_gpgsa_s *gsa) {
  // fixtype: 1 = no fix, 2 = 2D, 3 = 3D  (stored as int by the parser)
  if (gsa->fixtype == 3) {
    _data.fix.fix_type = GpsFixType::Fix3D;
  } else if (gsa->fixtype == 2) {
    _data.fix.fix_type = GpsFixType::Fix2D;
  } else {
    _data.fix.fix_type = GpsFixType::NoFix;
  }

  _data.fix.pdop = static_cast<float>(gsa->pdop);
  _data.fix.hdop = static_cast<float>(gsa->hdop);
  _data.fix.vdop = static_cast<float>(gsa->vdop);
}

double GpsDriver::_to_decimal_degrees(const nmea_position &pos) {
  double dd = static_cast<double>(pos.degrees) + pos.minutes / 60.0;
  if (pos.cardinal == NMEA_CARDINAL_DIR_SOUTH || pos.cardinal == NMEA_CARDINAL_DIR_WEST) {
    dd = -dd;
  }
  return dd;
}
