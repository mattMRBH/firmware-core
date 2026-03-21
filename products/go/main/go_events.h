#ifndef GO_EVENTS_H
#define GO_EVENTS_H

#include <cstdint>

#include "go_types.h"
#include "measures_types.h"

// --- Event type discriminator ---

enum class EventType : uint8_t {
  // --- Producer events ---
  SensorDataReady, // payload: Measures
  GpsFixUpdate,    // payload: GpsData
  InputPress,      // payload: InputEventData

  // --- System events ---
  InactivityTimeout, // no payload
  MeasurementTimer,  // no payload
  WakeFromSleep,     // payload: WakeEventData

  // --- UI action events ---
  UserStartTracking, // no payload
  UserStopTracking,  // no payload
  UserChangeMode,    // payload: OperatingMode
  UserToggleGps,     // payload: bool
  SettingsChanged,   // no payload
};

// --- Event payload structs ---

struct InputEventData {
  InputSource source;
  InputType type;
};

struct WakeEventData {
  WakeCause cause;
};

// --- Event struct ---
//
// Fixed-size struct with a type discriminator and a union of all possible
// payloads. Union size is dominated by Measures (~160 bytes). Events with no
// payload only use the type field; the union is allocated but not accessed.
//
// Note: always initialize with braces (e.g. Event evt{}) rather than plain
// declaration, because GpsData default member initializers make the anonymous
// union's default constructor deleted.

struct Event {
  EventType type;

  union {
    Measures sensor_data;      // SensorDataReady (~160 bytes)
    GpsData gps_data;          // GpsFixUpdate (~34 bytes)
    InputEventData input;      // InputPress (2 bytes)
    OperatingMode mode_change; // UserChangeMode (1 byte)
    WakeEventData wake;        // WakeFromSleep (1 byte)
    bool gps_enabled;          // UserToggleGps (1 byte)
  };
};

// Queue memory footprint: EVENT_QUEUE_DEPTH * sizeof(Event) ~ 2.6 KB
static constexpr uint8_t EVENT_QUEUE_DEPTH = 16;

#endif // GO_EVENTS_H
