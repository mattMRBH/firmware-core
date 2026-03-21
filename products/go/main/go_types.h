#ifndef GO_TYPES_H
#define GO_TYPES_H

#include <cstdint>
#include <ctime>

// --- GPS sentinel values ---

namespace GpsInvalid {
constexpr double LATITUDE = 91.0;   // valid range: -90 to 90
constexpr double LONGITUDE = 181.0; // valid range: -180 to 180
constexpr float ALTITUDE = -10000.0f;
constexpr float SPEED = -1.0f;
} // namespace GpsInvalid

struct GpsData {
  double latitude = GpsInvalid::LATITUDE;
  double longitude = GpsInvalid::LONGITUDE;
  float altitude = GpsInvalid::ALTITUDE;
  float speed = GpsInvalid::SPEED; // m/s
  uint8_t satellites = 0;
  bool fix_valid = false;
  time_t utc_timestamp = 0; // UTC epoch seconds from GPS RMC sentence
  bool time_valid = false;  // true when full date+time available

  bool is_position_valid() const;
  bool is_altitude_valid() const;
  bool is_speed_valid() const;
};

// --- Operating mode and behavior ---

enum class OperatingMode : uint8_t {
  Portable,
  Stationary,
  Offline,
};

enum class Behavior : uint8_t {
  Tracking,
  Idle,
  Shutdown,
};

enum class LockState : uint8_t {
  Locked,
  Unlocked,
};

enum class WakeCause : uint8_t {
  PowerOn, // fresh boot or power-on reset
  Timer,   // deep sleep timer expiry
  Button,  // GPIO wake from deep sleep
};

enum class InputSource : uint8_t {
  TouchUp,
  TouchDown,
  TouchEnter,
  ButtonPower,
  ButtonBoot,
};

enum class InputType : uint8_t {
  ShortPress,
  LongPress,
};

// --- RTC-persisted application state ---
//
// Saved to RTC memory before deep sleep; restored on wake to resume application
// state without full re-initialization. Defaults represent a safe starting
// point for a fresh power-on (Offline, Idle, Locked).

struct RtcAppState {
  OperatingMode mode = OperatingMode::Offline;
  Behavior behavior = Behavior::Idle;
  LockState lock_state = LockState::Locked;
  bool gps_enabled = true;
  bool tracking_active = false;
};

#endif // GO_TYPES_H
