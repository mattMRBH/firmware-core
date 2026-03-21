/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "hal/gps_sensor.h"
#include "types/gps_types.h"

TEST_CASE("airgradient-gps scaffold is present", "[airgradient-gps]") { SUCCEED(); }

TEST_CASE("airgradient-gps public types expose invalid sentinels", "[airgradient-gps]") {
  const GpsData data;

  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(data.position.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE(data.altitude_m == GPS_ALTITUDE_INVALID);
  REQUIRE(data.fix.fix_type == GpsFixType::NoFix);
  REQUIRE(data.fix.satellite_count == GPS_SATELLITE_COUNT_INVALID);
  REQUIRE(data.fix.hdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.pdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.vdop == GPS_DOP_INVALID);
  REQUIRE_FALSE(data.timestamp.valid);
}

TEST_CASE("airgradient-gps validation helpers reject sentinel values", "[airgradient-gps]") {
  REQUIRE_FALSE(is_latitude_valid(GPS_LATITUDE_INVALID));
  REQUIRE_FALSE(is_longitude_valid(GPS_LONGITUDE_INVALID));
  REQUIRE_FALSE(is_altitude_valid(GPS_ALTITUDE_INVALID));
  REQUIRE_FALSE(is_satellite_count_valid(GPS_SATELLITE_COUNT_INVALID));
}

TEST_CASE("airgradient-gps validation helpers accept in-range values", "[airgradient-gps]") {
  REQUIRE(is_latitude_valid(0.0));
  REQUIRE(is_latitude_valid(90.0));
  REQUIRE(is_latitude_valid(-90.0));
  REQUIRE(is_longitude_valid(0.0));
  REQUIRE(is_longitude_valid(180.0));
  REQUIRE(is_longitude_valid(-180.0));
  REQUIRE(is_altitude_valid(0.0f));
  REQUIRE(is_altitude_valid(-430.0f)); // Dead Sea
  REQUIRE(is_satellite_count_valid(0));
  REQUIRE(is_satellite_count_valid(12));
}
