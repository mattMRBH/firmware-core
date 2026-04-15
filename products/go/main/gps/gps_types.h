/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

// Invalid sentinel values — used to mark uninitialized or unavailable fields.
// Valid ranges are enforced by the is_*_valid() free functions below.
inline constexpr double GPS_LATITUDE_INVALID = 91.0;     // valid: [-90, 90]
inline constexpr double GPS_LONGITUDE_INVALID = 181.0;   // valid: [-180, 180]
inline constexpr float GPS_ALTITUDE_INVALID = -10000.0f; // valid: > GPS_ALTITUDE_INVALID
inline constexpr int GPS_SATELLITE_COUNT_INVALID = -1;   // valid: >= 0
inline constexpr float GPS_DOP_INVALID = -1.0f;          // valid: > 0

enum class GpsFixType : uint8_t {
  NoFix = 0,
  Fix2D = 2,
  Fix3D = 3,
};

struct GpsPosition {
  double latitude = GPS_LATITUDE_INVALID;   // decimal degrees, + = North, - = South
  double longitude = GPS_LONGITUDE_INVALID; // decimal degrees, + = East,  - = West
};

struct GpsFix {
  GpsFixType fix_type = GpsFixType::NoFix;
  int satellite_count = GPS_SATELLITE_COUNT_INVALID;
  float hdop = GPS_DOP_INVALID;
  float pdop = GPS_DOP_INVALID;
  float vdop = GPS_DOP_INVALID;
};

struct GpsTimestamp {
  int year = 0;   // full year, e.g. 2026
  int month = 0;  // 1–12
  int day = 0;    // 1–31
  int hour = 0;   // 0–23
  int minute = 0; // 0–59
  int second = 0; // 0–59
  bool valid = false;
};

struct GpsData {
  GpsPosition position;
  float altitude_m = GPS_ALTITUDE_INVALID; // meters above mean sea level
  GpsFix fix;
  GpsTimestamp timestamp;
};

// ---------------------------------------------------------------------------
// Field-level validation helpers
// ---------------------------------------------------------------------------

inline bool is_latitude_valid(double lat) { return lat >= -90.0 && lat <= 90.0; }

inline bool is_longitude_valid(double lon) { return lon >= -180.0 && lon <= 180.0; }

inline bool is_position_valid(const GpsPosition &pos) {
  return is_latitude_valid(pos.latitude) && is_longitude_valid(pos.longitude);
}

inline bool is_altitude_valid(float alt) { return alt > GPS_ALTITUDE_INVALID; }

inline bool is_satellite_count_valid(int count) { return count >= 0; }

inline bool is_fix_valid(const GpsFix &fix) { return fix.fix_type != GpsFixType::NoFix; }

inline bool is_gps_timestamp_valid(const GpsTimestamp &ts) { return ts.valid; }
