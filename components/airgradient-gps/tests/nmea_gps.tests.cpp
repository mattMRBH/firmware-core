/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>

#include "drivers/nmea_gps/nmea_gps.h"

// ---------------------------------------------------------------------------
// StubSerial — minimal AirgradientSerial that serves data from a byte queue.
// ---------------------------------------------------------------------------
class StubSerial : public AirgradientSerial {
public:
  void queue_rx(const char *data) {
    while (data != nullptr && *data != '\0') {
      _rx.push_back(static_cast<uint8_t>(*data));
      ++data;
    }
  }

  bool begin(int /*baud*/) override { return true; }
  void end() override {}

  int available() override { return static_cast<int>(_rx.size()); }

  int read() override {
    if (_rx.empty()) {
      return -1;
    }
    const uint8_t val = _rx.front();
    _rx.pop_front();
    return val;
  }

  void print(const char * /*str*/) override {}
  int write(const uint8_t * /*data*/, int len) override { return len; }

private:
  std::deque<uint8_t> _rx;
};

// ---------------------------------------------------------------------------
// TestableNmeaGps — exposes the private _to_decimal_degrees helper.
// ---------------------------------------------------------------------------
class TestableNmeaGps : public NmeaGps {
public:
  using NmeaGps::NmeaGps;

  static double test_to_decimal_degrees(const nmea_position &pos) {
    return NmeaGps::_to_decimal_degrees(pos);
  }
};

// Reference NMEA sentences with verified correct checksums.
// Checksums computed as XOR of all bytes between '$' and '*' (exclusive).
// Note: the spec's listed checksums (5B, 5A, 57, 0A) were incorrect;
// the values below were verified programmatically.
//
// GGA: position_fix=1, lat=4717.11364N, lon=00833.91565E, alt=499.6, sats=8
// XOR("GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,") = 0x53
static constexpr const char *GGA_VALID =
    "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*53\r\n";

// Same sentence but position_fix=0 (no fix).
// Checksum: 0x53 ^ ord('1') ^ ord('0') = 0x52
static constexpr const char *GGA_NO_FIX =
    "$GPGGA,092725.00,4717.11364,N,00833.91565,E,0,08,1.01,499.6,M,48.0,M,,*52\r\n";

// RMC: status=A (valid), same lat/lon, date=091202, time=092725
// XOR("GPRMC,092725.00,A,4717.11364,N,00833.91565,E,0.004,77.52,091202,,,A") = 0x5C
static constexpr const char *RMC_VALID =
    "$GPRMC,092725.00,A,4717.11364,N,00833.91565,E,0.004,77.52,091202,,,A*5C\r\n";

// GSA: fix=3 (3D), pdop=1.94, hdop=1.01, vdop=1.65
// XOR("GPGSA,A,3,23,29,07,08,09,18,26,28,,,,,1.94,1.01,1.65") = 0x07
static constexpr const char *GSA_VALID =
    "$GPGSA,A,3,23,29,07,08,09,18,26,28,,,,,1.94,1.01,1.65*07\r\n";

// GGA with a deliberately wrong checksum (*FF instead of the correct *53).
static constexpr const char *GGA_BAD_CHECKSUM =
    "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*FF\r\n";

// Expected parsed values.
static constexpr double EXPECTED_LAT = 47.0 + 17.11364 / 60.0; // ~47.285227
static constexpr double EXPECTED_LON = 8.0 + 33.91565 / 60.0;  // ~8.565261
static constexpr float EXPECTED_ALT = 499.6f;
static constexpr int EXPECTED_SATS = 8;
static constexpr float EXPECTED_HDOP = 1.01f;
static constexpr float EXPECTED_PDOP = 1.94f;
static constexpr float EXPECTED_VDOP = 1.65f;
static constexpr int EXPECTED_YEAR = 2002;
static constexpr int EXPECTED_MONTH = 12;
static constexpr int EXPECTED_DAY = 9;
static constexpr int EXPECTED_HOUR = 9;
static constexpr int EXPECTED_MINUTE = 27;
static constexpr int EXPECTED_SECOND = 25;

// ---------------------------------------------------------------------------
// Test 1 — Scaffold: component compiles and links under TEST_HOST.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps scaffold compiles and links under TEST_HOST", "[airgradient-gps][nmea-gps]") {
  SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 2 — GGA parsing.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps parses GGA sentence: position, altitude, satellite count",
          "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  serial.queue_rx(GGA_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));
  REQUIRE(data.altitude_m == Catch::Approx(EXPECTED_ALT).epsilon(0.1f));
  REQUIRE(data.fix.satellite_count == EXPECTED_SATS);
  REQUIRE(is_position_valid(data.position));
  REQUIRE(is_altitude_valid(data.altitude_m));
  REQUIRE(is_satellite_count_valid(data.fix.satellite_count));
}

// ---------------------------------------------------------------------------
// Test 3 — RMC parsing.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps parses RMC sentence: position and timestamp", "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  serial.queue_rx(RMC_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();
  // RMC should fill position when GGA has not been received yet.
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));

  REQUIRE(data.timestamp.valid);
  REQUIRE(data.timestamp.year == EXPECTED_YEAR);
  REQUIRE(data.timestamp.month == EXPECTED_MONTH);
  REQUIRE(data.timestamp.day == EXPECTED_DAY);
  REQUIRE(data.timestamp.hour == EXPECTED_HOUR);
  REQUIRE(data.timestamp.minute == EXPECTED_MINUTE);
  REQUIRE(data.timestamp.second == EXPECTED_SECOND);
  REQUIRE(is_gps_timestamp_valid(data.timestamp));
}

// ---------------------------------------------------------------------------
// Test 4 — GSA parsing.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps parses GSA sentence: fix type and DOP values", "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  serial.queue_rx(GSA_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.fix.fix_type == GpsFixType::Fix3D);
  REQUIRE(data.fix.hdop == Catch::Approx(EXPECTED_HDOP).epsilon(0.01f));
  REQUIRE(data.fix.pdop == Catch::Approx(EXPECTED_PDOP).epsilon(0.01f));
  REQUIRE(data.fix.vdop == Catch::Approx(EXPECTED_VDOP).epsilon(0.01f));
}

// ---------------------------------------------------------------------------
// Test 5 — Multi-sentence accumulation.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps accumulates GGA + RMC + GSA and populates all fields",
          "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  serial.queue_rx(GGA_VALID);
  serial.queue_rx(RMC_VALID);
  serial.queue_rx(GSA_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();

  // Position from GGA.
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));
  REQUIRE(data.altitude_m == Catch::Approx(EXPECTED_ALT).epsilon(0.1f));
  REQUIRE(data.fix.satellite_count == EXPECTED_SATS);

  // Timestamp from RMC.
  REQUIRE(data.timestamp.valid);
  REQUIRE(data.timestamp.year == EXPECTED_YEAR);
  REQUIRE(data.timestamp.month == EXPECTED_MONTH);

  // Fix type and DOP from GSA (overrides GGA's conservative Fix2D).
  REQUIRE(data.fix.fix_type == GpsFixType::Fix3D);
  REQUIRE(data.fix.hdop == Catch::Approx(EXPECTED_HDOP).epsilon(0.01f));
  REQUIRE(data.fix.pdop == Catch::Approx(EXPECTED_PDOP).epsilon(0.01f));
  REQUIRE(data.fix.vdop == Catch::Approx(EXPECTED_VDOP).epsilon(0.01f));

  REQUIRE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 6 — No fix.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps reports no valid fix when GGA position_fix is 0",
          "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  serial.queue_rx(GGA_NO_FIX);
  REQUIRE(gps.read());

  REQUIRE_FALSE(gps.has_valid_fix());
  REQUIRE(gps.get_data().fix.fix_type == GpsFixType::NoFix);
}

// ---------------------------------------------------------------------------
// Test 7 — Invalid checksum: data must remain at sentinels.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps ignores sentence with bad checksum", "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  serial.queue_rx(GGA_BAD_CHECKSUM);
  // Sentence boundary is reached (read returns true), but nmea_parse
  // rejects the invalid checksum, so state stays at sentinels.
  gps.read();

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(data.position.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE(data.altitude_m == GPS_ALTITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 8 — Partial sentence: no crash, no state change.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps handles partial sentence without crashing or changing state",
          "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  // Feed the first 30 characters of a valid GGA sentence (no \r\n yet).
  const std::string partial(GGA_VALID, 30);
  serial.queue_rx(partial.c_str());
  gps.read();

  // No complete sentence has been delivered — state must remain at sentinels.
  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 9 — Buffer overflow: graceful discard of oversized input.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps discards input that overflows the accumulation buffer",
          "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  // Build a fake "sentence" that far exceeds the 256-byte buffer:
  // '$' followed by 300 uppercase-A characters (no \r\n).
  std::string oversized;
  oversized += '$';
  oversized.append(300, 'A');
  serial.queue_rx(oversized.c_str());

  // Must not crash; buffer overflow path resets the accumulator.
  gps.read();

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 10 — Sentinel initialization.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps get_data returns all-invalid sentinels before any read",
          "[airgradient-gps][nmea-gps]") {
  StubSerial serial;
  NmeaGps gps(serial);

  const GpsData data = gps.get_data();

  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(data.position.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE(data.altitude_m == GPS_ALTITUDE_INVALID);
  REQUIRE(data.fix.fix_type == GpsFixType::NoFix);
  REQUIRE(data.fix.satellite_count == GPS_SATELLITE_COUNT_INVALID);
  REQUIRE(data.fix.hdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.pdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.vdop == GPS_DOP_INVALID);
  REQUIRE_FALSE(data.timestamp.valid);

  REQUIRE_FALSE(gps.has_valid_fix());
  REQUIRE_FALSE(is_position_valid(data.position));
  REQUIRE_FALSE(is_altitude_valid(data.altitude_m));
  REQUIRE_FALSE(is_satellite_count_valid(data.fix.satellite_count));
  REQUIRE_FALSE(is_fix_valid(data.fix));
  REQUIRE_FALSE(is_gps_timestamp_valid(data.timestamp));
}

// ---------------------------------------------------------------------------
// Test 11 — Coordinate conversion for all four cardinal directions.
// ---------------------------------------------------------------------------
TEST_CASE("nmea_gps to_decimal_degrees converts N/S/E/W cardinal directions",
          "[airgradient-gps][nmea-gps]") {
  nmea_position pos{};

  // North: 47° 17.11364' N → +47.285227...
  pos.degrees = 47;
  pos.minutes = 17.11364;
  pos.cardinal = NMEA_CARDINAL_DIR_NORTH;
  REQUIRE(TestableNmeaGps::test_to_decimal_degrees(pos) ==
          Catch::Approx(47.0 + 17.11364 / 60.0).epsilon(1e-9));

  // South: 47° 17.11364' S → -47.285227...
  pos.cardinal = NMEA_CARDINAL_DIR_SOUTH;
  REQUIRE(TestableNmeaGps::test_to_decimal_degrees(pos) ==
          Catch::Approx(-(47.0 + 17.11364 / 60.0)).epsilon(1e-9));

  // East: 8° 33.91565' E → +8.565261...
  pos.degrees = 8;
  pos.minutes = 33.91565;
  pos.cardinal = NMEA_CARDINAL_DIR_EAST;
  REQUIRE(TestableNmeaGps::test_to_decimal_degrees(pos) ==
          Catch::Approx(8.0 + 33.91565 / 60.0).epsilon(1e-9));

  // West: 8° 33.91565' W → -8.565261...
  pos.cardinal = NMEA_CARDINAL_DIR_WEST;
  REQUIRE(TestableNmeaGps::test_to_decimal_degrees(pos) ==
          Catch::Approx(-(8.0 + 33.91565 / 60.0)).epsilon(1e-9));
}
