#ifndef GO_TYPES_H
#define GO_TYPES_H

#include <cstdint>

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

enum class GpsMode : uint8_t {
  AlwaysOff,
  OnWhenTracking,
  AlwaysOn,
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
  uint32_t tracking_session_id = 0; ///< 5-digit session ID; 0 = no active session
};

#endif // GO_TYPES_H
