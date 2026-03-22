# Common Types — Implementation Spec

This spec defines the shared data types and event system for the AirGradient Go
product. All other product-specific services depend on these types.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_types.h` | Product-specific data types |
| `products/go/main/go_types.cpp` | GpsData validation method implementations |
| `products/go/main/go_events.h` | Event system (EventType, payloads, Event struct) |

## go_types.h

### GpsData

Follows the existing sensor data pattern: invalid sentinel defaults and
per-field validation methods.

```cpp
#pragma once

#include <cstdint>
#include <ctime>

// --- GPS types ---

namespace GpsInvalid {
constexpr double LATITUDE  = 91.0;     // valid range: -90 to 90
constexpr double LONGITUDE = 181.0;    // valid range: -180 to 180
constexpr float ALTITUDE   = -10000.0f;
constexpr float SPEED      = -1.0f;
}  // namespace GpsInvalid

struct GpsData {
    double latitude    = GpsInvalid::LATITUDE;
    double longitude   = GpsInvalid::LONGITUDE;
    float altitude     = GpsInvalid::ALTITUDE;
    float speed        = GpsInvalid::SPEED;       // m/s
    uint8_t satellites = 0;
    bool fix_valid     = false;
    time_t utc_timestamp = 0;   // UTC epoch seconds from GPS RMC sentence
    bool time_valid      = false; // true when full date+time available

    bool is_position_valid() const;
    bool is_altitude_valid() const;
    bool is_speed_valid() const;
};
```

**Validation rules:**
- `is_position_valid()`: `fix_valid && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180`
- `is_altitude_valid()`: `altitude > GpsInvalid::ALTITUDE`
- `is_speed_valid()`: `speed >= 0.0f`

### Enums

```cpp
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
    PowerOn,  // fresh boot or power-on reset
    Timer,    // deep sleep timer expiry
    Button,   // GPIO wake from deep sleep
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
```

### RtcAppState

Persisted to RTC memory before deep sleep. Restored on wake to resume
application state without full re-initialization.

```cpp
struct RtcAppState {
    OperatingMode mode       = OperatingMode::Offline;
    Behavior behavior        = Behavior::Idle;
    LockState lock_state     = LockState::Locked;
    bool gps_enabled         = true;
    bool tracking_active     = false;
};
```

All fields use safe defaults: Offline mode, Idle behavior, Locked, GPS on,
not tracking. A fresh power-on uses these defaults. A deep-sleep wake restores
the previously saved state.

## go_events.h

### EventType

```cpp
#pragma once

#include "go_types.h"
#include "measures_types.h"

#include <cstdint>

enum class EventType : uint8_t {
    // --- Producer events ---
    SensorDataReady,       // payload: Measures
    GpsFixUpdate,          // payload: GpsData
    InputPress,            // payload: InputEventData

    // --- System events ---
    InactivityTimeout,     // no payload
    MeasurementTimer,      // no payload
    WakeFromSleep,         // payload: WakeEventData

    // --- UI action events ---
    UserStartTracking,     // no payload
    UserStopTracking,      // no payload
    UserChangeMode,        // payload: OperatingMode
    UserToggleGps,         // payload: bool
    SettingsChanged,       // no payload
};
```

### Payload Structs

Small structs for events that carry typed data beyond a single enum/bool:

```cpp
struct InputEventData {
    InputSource source;
    InputType type;
};

struct WakeEventData {
    WakeCause cause;
};
```

### Event Struct

Fixed-size struct with a type discriminator and a union of all possible
payloads. The union size is dominated by `Measures` (~160 bytes).

```cpp
struct Event {
    EventType type;

    union {
        Measures sensor_data;          // SensorDataReady (~160 bytes)
        GpsData gps_data;             // GpsFixUpdate (~34 bytes)
        InputEventData input;          // InputPress (2 bytes)
        OperatingMode mode_change;     // UserChangeMode (1 byte)
        WakeEventData wake;            // WakeFromSleep (1 byte)
        bool gps_enabled;             // UserToggleGps (1 byte)
    };
};
```

Events with no payload (`InactivityTimeout`, `MeasurementTimer`,
`UserStartTracking`, `UserStopTracking`, `SettingsChanged`) only use the `type`
field. The union is always allocated but not accessed for these event types.

### Queue Constant

```cpp
static constexpr uint8_t EVENT_QUEUE_DEPTH = 16;
```

Queue memory footprint: `16 * sizeof(Event)` ~ 2.6 KB.

## Design Decisions

### Measures in the Event Union

`SensorManager::start_measures()` returns the full `Measures` struct regardless
of which sensors are wired. AGo has the basic sensor set (PM + CO2 + TempHum +
TVOC/NOx), so unused fields in `Measures` carry sentinel values. No conversion
to `MeasuresAGo` in the event path. Storage Service can decide independently
whether to persist full or basic structs.

### SettingsChanged Instead of Encoding Key+Value

When the user changes a setting via the UI menu, the UI manager writes the new
value directly to the Settings service, then posts a `SettingsChanged` event
with no payload. The orchestrator re-reads whichever settings it cares about
(measurement interval, display refresh, GPS enabled, etc.). This avoids the
complexity of encoding arbitrary key+value pairs in the event union.

### GPS Time as System Clock Source

NMEA RMC sentences provide full UTC date and time from GPS satellite atomic
clocks. The `GpsData.utc_timestamp` field carries this as epoch seconds when
available (`time_valid == true`).

The orchestrator should sync the ESP32 system clock to GPS UTC on the first
valid fix that includes time. This provides accurate absolute timestamps for
route logging without requiring NTP or WiFi connectivity. Subsequent GPS time
updates can periodically re-sync the system clock to prevent drift.

### GPS Fix Posting Rate

The GPS task does not post every parsed NMEA sentence (typically 1 Hz). Instead,
it posts `GpsFixUpdate` events at a configurable interval. The interval is a
product setting (default TBD, likely 1-5 seconds). This prevents flooding the
event queue while still providing adequate position resolution for route
tracking.

### No Timestamp in Event Struct

Individual events do not carry a generic timestamp field. Route logging
timestamps use the system clock (synced from GPS) at the time of logging.
GPS fixes carry their own `utc_timestamp` from the satellite signal.

## Dependencies

- `go_types.h`: standalone (`<cstdint>`, `<ctime>`)
- `go_events.h`: `go_types.h` + `measures_types.h` (from `airgradient-common`)

## Testability

Both headers are plain data types with no ESP-IDF dependency. They compile
under `TEST_HOST` without any `#ifdef` guards. `GpsData` validation methods
are implemented in `go_types.cpp` with no platform-specific calls.
