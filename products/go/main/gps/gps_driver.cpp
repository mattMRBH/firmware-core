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
#include <cstring>
#include <ctime>

#include "rtos.h"

static constexpr const char *TAG = "GpsDriver";

// Time to wait after sending the baud-rate switch command.  The 18-byte
// command takes ~19 ms to transmit at 9600 baud (18 * 10 bits / 9600);
// 50 ms provides margin for TX drain and module processing.
static constexpr uint32_t BAUD_SWITCH_DELAY_MS = 50;

// TAU1113 CASIC binary command: UART configure as 115200 bps.
// Source: TAU1113 Datasheet v1.4, Table 26 (section 8.2).
// Note [2]: 0D 0A appended at end of command.
// clang-format off
static constexpr uint8_t CMD_BAUD_115200[] = {
    0xF1, 0xD9, 0x06, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xC2, 0x01, 0x00, 0xD1, 0xE0,
    0x0D, 0x0A,
};
// clang-format on

// ---------------------------------------------------------------------------
// CASIC binary protocol helpers (file-local)
// ---------------------------------------------------------------------------

/// Compute the CASIC 8-Bit Fletcher checksum.
/// @param data  Pointer to the first byte AFTER the 0xF1 0xD9 header
///              (i.e., starting at group ID).
/// @param len   Number of bytes: 2 (IDs) + 2 (length) + payload_len.
static void casic_checksum(const uint8_t *data, size_t len, uint8_t &ck1, uint8_t &ck2) {
  ck1 = 0;
  ck2 = 0;
  for (size_t i = 0; i < len; ++i) {
    ck1 = (ck1 + data[i]) & 0xFF;
    ck2 = (ck2 + ck1) & 0xFF;
  }
}

/// Maximum payload size across AID-POS (17), AID-TIME (20), CFG-EPHSAVE (1).
static constexpr size_t CASIC_MAX_PAYLOAD = 20;

/// Build and send a CASIC binary packet.
/// Stack buffer: 2 (header) + 2 (ID) + 2 (length) + payload + 2 (checksum)
///             = 28 bytes max.
static void send_casic_packet(AirgradientSerial &serial, uint8_t group, uint8_t sub,
                              const uint8_t *payload, uint16_t payload_len) {
  uint8_t buf[2 + 2 + 2 + CASIC_MAX_PAYLOAD + 2];
  buf[0] = 0xF1;
  buf[1] = 0xD9;
  buf[2] = group;
  buf[3] = sub;
  buf[4] = static_cast<uint8_t>(payload_len & 0xFF);
  buf[5] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
  memcpy(&buf[6], payload, payload_len);

  uint8_t ck1, ck2;
  casic_checksum(&buf[2], 4 + payload_len, ck1, ck2);
  buf[6 + payload_len] = ck1;
  buf[7 + payload_len] = ck2;

  serial.write(buf, 8 + payload_len);
}

/// Leap seconds since 1980 (18 as of 2026, unchanged since 2017-01-01).
static constexpr uint8_t GPS_LEAP_SECONDS_SINCE_1980 = 18;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GpsDriver::GpsDriver(AirgradientSerial &serial) : _serial(serial), _buffer_pos(0) {}

GpsDriver::~GpsDriver() {}

bool GpsDriver::begin(int baud_rate) {
  if (baud_rate != MODULE_DEFAULT_BAUD) {
    // The TAU1113 defaults to 9600 baud on power-on.  Open at the module's
    // default rate, send the binary command to switch to 115200, then
    // re-initialise the UART at the target rate.  If the module is already
    // at 115200 (stayed powered during deep sleep) the 9600-baud bytes
    // arrive as garbage and are silently ignored.
    if (!_serial.begin(MODULE_DEFAULT_BAUD)) {
      return false;
    }
    _send_baud_115200_command();
    RTOS::delay_ms(BAUD_SWITCH_DELAY_MS);
    _serial.end();
  }
  if (!_serial.begin(baud_rate)) {
    return false;
  }
  _send_cfg_ephsave();
  return true;
}

void GpsDriver::end() { _serial.end(); }

bool GpsDriver::read() {
  bool got_sentence = false;
  const int avail = _serial.available();
  if (avail <= 0) {
    return false;
  }

  uint8_t chunk[READ_CHUNK_SIZE];
  const int to_read =
      (avail < static_cast<int>(sizeof(chunk))) ? avail : static_cast<int>(sizeof(chunk));
  const int n = _serial.read(chunk, to_read);
  for (int i = 0; i < n; i++) {
    if (_process_byte(static_cast<char>(chunk[i]))) {
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
    if (_is_accepted_sentence(_buffer, _buffer_pos)) {
      _handle_sentence(_buffer, _buffer_pos);
      _buffer_pos = 0;
      return true;
    }
    // Drop filtered sentence silently -- no allocator activity.
    _buffer_pos = 0;
    return false;
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

void GpsDriver::_send_baud_115200_command() {
  _serial.write(CMD_BAUD_115200, sizeof(CMD_BAUD_115200));
}

bool GpsDriver::_is_accepted_sentence(const char *buf, size_t len) {
  // Minimum viable sentence: "$XXYYY" = 6 chars before any fields.
  if (len < 6) {
    return false;
  }
  const char *id = buf + 3; // skip "$" + 2-char talker
  return (memcmp(id, "GGA", 3) == 0) || (memcmp(id, "RMC", 3) == 0) || (memcmp(id, "GSA", 3) == 0);
}

// ---------------------------------------------------------------------------
// A-GNSS aiding injection
// ---------------------------------------------------------------------------

void GpsDriver::inject_aiding(const GpsAidingData &data) {
  if (has_aiding_position(data)) {
    const float alt = is_altitude_valid(data.altitude_m) ? data.altitude_m : 0.0f;
    _send_aid_pos(data.latitude, data.longitude, alt, data.pos_acc_m);
  }
  if (has_aiding_time(data)) {
    _send_aid_time(data.epoch_s, data.time_acc_ms);
  }
}

void GpsDriver::_send_aid_pos(double lat_deg, double lon_deg, float alt_m, float pos_acc_m) {
  // AID-POS payload: 17 bytes
  // [0]     U1  type      = 1 (LLA)
  // [1-4]   S4  lat       degrees * 1e7
  // [5-8]   S4  lon       degrees * 1e7
  // [9-12]  S4  alt       centimeters
  // [13-16] U4  pos_acc   centimeters
  uint8_t payload[17];
  memset(payload, 0, sizeof(payload));

  payload[0] = 0x01; // type = LLA

  const auto lat_scaled = static_cast<int32_t>(lat_deg * 1e7);
  const auto lon_scaled = static_cast<int32_t>(lon_deg * 1e7);
  const auto alt_cm = static_cast<int32_t>(alt_m * 100.0f);
  const auto acc_cm = static_cast<uint32_t>(pos_acc_m * 100.0f);

  memcpy(&payload[1], &lat_scaled, 4);
  memcpy(&payload[5], &lon_scaled, 4);
  memcpy(&payload[9], &alt_cm, 4);
  memcpy(&payload[13], &acc_cm, 4);

  send_casic_packet(_serial, 0x0B, 0x10, payload, sizeof(payload));
}

void GpsDriver::_send_aid_time(int64_t epoch_s, uint32_t time_acc_ms) {
  // Convert epoch to UTC calendar fields.
  const auto t = static_cast<time_t>(epoch_s);
  struct tm utc{};
  gmtime_r(&t, &utc);

  // AID-TIME payload: 20 bytes (UTC variant)
  // [0]     U1  type       = 0 (UTC)
  // [1]     U1  reserved   = 0
  // [2]     U1  leap_sec   leap seconds since 1980
  // [3-4]   U2  year
  // [5]     U1  month
  // [6]     U1  day
  // [7]     U1  hour
  // [8]     U1  minute
  // [9]     U1  second
  // [10-13] U4  sec_ns     nanoseconds portion (0 for us)
  // [14-15] U2  tacc_s     integer seconds of accuracy
  // [16-19] U4  tacc_ns    nanosecond portion of accuracy
  uint8_t payload[20];
  memset(payload, 0, sizeof(payload));

  payload[0] = 0x00; // type = UTC
  // payload[1] reserved = 0
  payload[2] = GPS_LEAP_SECONDS_SINCE_1980;

  const auto year = static_cast<uint16_t>(utc.tm_year + 1900);
  memcpy(&payload[3], &year, 2);
  payload[5] = static_cast<uint8_t>(utc.tm_mon + 1);
  payload[6] = static_cast<uint8_t>(utc.tm_mday);
  payload[7] = static_cast<uint8_t>(utc.tm_hour);
  payload[8] = static_cast<uint8_t>(utc.tm_min);
  payload[9] = static_cast<uint8_t>(utc.tm_sec);
  // payload[10-13] sec_ns = 0 (no sub-second precision available)

  const auto tacc_s = static_cast<uint16_t>(time_acc_ms / 1000);
  const auto tacc_ns = static_cast<uint32_t>((time_acc_ms % 1000) * 1000000);
  memcpy(&payload[14], &tacc_s, 2);
  memcpy(&payload[16], &tacc_ns, 4);

  send_casic_packet(_serial, 0x0B, 0x11, payload, sizeof(payload));
}

void GpsDriver::_send_cfg_ephsave() {
  // CFG-EPHSAVE payload: 1 byte
  // [0]  U1  enable = 1
  const uint8_t payload[] = {0x01};
  send_casic_packet(_serial, 0x06, 0x10, payload, sizeof(payload));
}
