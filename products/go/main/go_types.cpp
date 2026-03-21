#include "go_types.h"

bool GpsData::is_position_valid() const {
  return fix_valid && latitude >= -90 && latitude <= 90 && longitude >= -180 &&
         longitude <= 180;
}

bool GpsData::is_altitude_valid() const {
  return altitude > GpsInvalid::ALTITUDE;
}

bool GpsData::is_speed_valid() const { return speed >= 0.0f; }
