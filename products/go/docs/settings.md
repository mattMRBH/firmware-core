# Settings Service

Persistent device configuration for AirGradient Go, stored in NVS via
`ConfigStore`. Follows the same pattern as the reference product
(`ReferenceSettings`): a plain struct with default member initializers and free
functions for load/save.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_settings.h` | `GoSettings` struct and load/save declarations |
| `products/go/main/go_settings.cpp` | NVS key constants, validation functions, load/save implementations |

## GoSettings Fields

| Field | NVS Key | Type | Default | Valid Range | Notes |
|---|---|---|---|---|---|
| `measurement_interval_seconds` | `"mis"` | `int` | `60` | 1 .. 3600 | How often to trigger a sensor measurement cycle |
| `display_refresh_interval_seconds` | `"dri"` | `int` | `60` | 0 .. 3600 | E-paper refresh rate while locked; `0` disables periodic refresh |
| `inactivity_timeout_seconds` | `"ito"` | `int` | `30` | 5 .. 600 | Auto-lock delay after no input while unlocked |
| `gps_interval_seconds` | `"gis"` | `int` | `5` | 1 .. 60 | How often the GPS task posts fixes to the event queue |
| `gps_enabled` | `"gen"` | `bool` | `true` | — | Software enable/disable for GPS data processing |
| `operating_mode` | `"opm"` | `int` (stored) / `OperatingMode` (in struct) | `Offline` (0) | 0 .. 2 | Serialized as int; cast to `OperatingMode` on load |
| `device_name` | `"dn"` | `std::string` | `"airgradient-go"` | 1 .. 64 chars | Advertised name for BLE/WiFi |

## Load Behavior

`load_go_settings(ConfigStore &store)` reads each NVS key in sequence. For
every field:

1. Call the appropriate `store.get_*()` for the key.
2. If the read succeeds **and** the value passes field-level validation: use the
   stored value.
3. If the read fails (key absent) or validation rejects the value: keep the
   struct default.

The function never fails. It always returns a fully populated `GoSettings`
struct with either the stored or the default value for each field.

## Save Behavior

`save_go_settings(ConfigStore &store, const GoSettings &settings)` validates
all fields up front before writing anything. If **any** field is out of range,
the function returns `false` immediately without touching NVS.

On success, all fields are written with `store.set_*()`, and `store.commit()`
is called. The function returns `true` only when all writes and the commit
succeed.

## Validation Rules

All validation is implemented in an anonymous namespace in `go_settings.cpp`
(file-local; not visible to callers).

| Field | Rule |
|---|---|
| `measurement_interval_seconds` | `>= 1 && <= 3600` |
| `display_refresh_interval_seconds` | `>= 0 && <= 3600` (`0` is valid — disables refresh) |
| `inactivity_timeout_seconds` | `>= 5 && <= 600` |
| `gps_interval_seconds` | `>= 1 && <= 60` |
| `gps_enabled` | No range check (bool) |
| `operating_mode` | Underlying int in `0 .. 2` (matches `OperatingMode` enum values) |
| `device_name` | Non-empty and `<= 64` characters |

## Relationship to RtcAppState

Some settings fields have a corresponding field in `RtcAppState` (the
RTC-memory cache used to survive deep sleep):

| Setting | `RtcAppState` field | Purpose |
|---|---|---|
| `operating_mode` | `mode` | Settings = durable (NVS, survives power-off). RtcAppState = fast cache (RTC memory, survives deep sleep only). |
| `gps_enabled` | `gps_enabled` | Same relationship. |

**Startup logic:**
- **Fresh power-on:** Load from `GoSettings` (NVS), then initialize
  `RtcAppState` from those values.
- **Deep sleep timer wake:** Read `RtcAppState` directly; skip NVS to keep the
  fast path minimal.
- **Mode or GPS toggle:** Write both `GoSettings` (NVS) and `RtcAppState`.

## Usage Example

```cpp
#include "go_settings.h"

// Provided by the NVS backend (platform-specific):
NvsConfigStore store("go_cfg");

// Load — always succeeds, falls back to defaults for missing/invalid keys.
GoSettings settings = load_go_settings(store);

// Mutate a field.
settings.measurement_interval_seconds = 120;

// Save — returns false if any field is invalid.
if (!save_go_settings(store, settings)) {
    ESP_LOGE(TAG, "save_go_settings: invalid settings, not written");
}
```

## Testability

`load_go_settings` and `save_go_settings` depend only on the `ConfigStore`
abstract interface. In host tests, inject a mock `ConfigStore` (e.g., via
Trompeloeil) that returns controlled values. No ESP-IDF dependency exists in
the settings logic itself.

Recommended test cases:

- Load with empty store returns all defaults.
- Load with valid stored values returns the stored values.
- Load with out-of-range stored values falls back to defaults.
- Save with valid settings returns `true` and calls `commit()`.
- Save with any single invalid field returns `false` without writing.
- Round-trip: save then load returns identical values.

## Dependencies

- `airgradient-config` component — provides `ConfigStore` interface and the NVS
  backend.
- `go_types.h` — provides the `OperatingMode` enum.
