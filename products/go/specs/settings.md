# Settings Service — Implementation Spec

This spec defines the AGo product-specific settings. Follows the same pattern as
the reference product (`ReferenceSettings`): a plain struct with defaults, free
functions for load/save, field-level validation, and `ConfigStore` (NVS) as the
storage backend.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_settings.h` | `GoSettings` struct + load/save declarations |
| `products/go/main/go_settings.cpp` | NVS key mapping, validation, load/save implementations |

## GoSettings Struct

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "config_store.h"
#include "go_types.h"

struct GoSettings {
    int measurement_interval_seconds    = 60;
    int display_refresh_interval_seconds = 60;   // 0 = display refresh disabled
    int inactivity_timeout_seconds      = 30;
    int gps_interval_seconds            = 5;
    bool gps_enabled                    = true;
    OperatingMode operating_mode         = OperatingMode::Offline;
    std::string device_name             = "airgradient-go";
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);
```

## Settings Detail

| Field | NVS Key | Type | Default | Valid Range | Notes |
|---|---|---|---|---|---|
| `measurement_interval_seconds` | `"mis"` | int | 60 | 1 .. 3600 | How often to trigger a sensor measurement cycle |
| `display_refresh_interval_seconds` | `"dri"` | int | 60 | 0 .. 3600 | E-paper refresh while locked. 0 disables refresh entirely |
| `inactivity_timeout_seconds` | `"ito"` | int | 30 | 5 .. 600 | Auto-lock after no input while unlocked |
| `gps_interval_seconds` | `"gis"` | int | 5 | 1 .. 60 | How often the GPS task posts fixes to the event queue |
| `gps_enabled` | `"gen"` | bool | true | — | Software enable/disable for GPS data processing |
| `operating_mode` | `"opm"` | int | 0 (Offline) | 0 .. 2 | Stored as int, cast to `OperatingMode` enum |
| `device_name` | `"dnm"` | string | `"airgradient-go"` | non-empty, max 64 chars | Used for BLE advertising name and WiFi hostname |

## Validation Functions

All validation functions are file-local (anonymous namespace in `.cpp`):

```cpp
namespace {

bool is_measurement_interval_valid(int value) {
    return value >= 1 && value <= 3600;
}

bool is_display_refresh_interval_valid(int value) {
    return value >= 0 && value <= 3600;
}

bool is_inactivity_timeout_valid(int value) {
    return value >= 5 && value <= 600;
}

bool is_gps_interval_valid(int value) {
    return value >= 1 && value <= 60;
}

bool is_operating_mode_valid(int value) {
    return value >= 0 && value <= 2;
}

bool is_device_name_valid(const std::string &value) {
    return !value.empty() && value.size() <= 64;
}

}  // namespace
```

## Load Behavior

`load_go_settings()` reads each key from NVS. For each field:
1. Attempt `store.get_*()` for the key
2. If found and passes validation: use the stored value
3. If not found or invalid: keep the struct default

Returns a fully populated `GoSettings` with either stored or default values.
Never fails — always returns a valid struct.

## Save Behavior

`save_go_settings()` validates all fields up front. If any field is invalid,
returns `false` without writing anything. On success, writes all fields and
calls `store.commit()`.

## Relationship to RtcAppState

Some settings overlap with `RtcAppState` fields:

| Setting | RtcAppState Field | Relationship |
|---|---|---|
| `operating_mode` | `mode` | RtcAppState is the fast cache for deep sleep. Settings is the durable store for power-off. |
| `gps_enabled` | `gps_enabled` | Same relationship. |

On fresh power-on: load from Settings (NVS) and initialize `RtcAppState`.
On deep sleep wake: use `RtcAppState` directly (skip NVS read on fast-path).
On mode/GPS change: update both Settings (NVS) and `RtcAppState`.

## Dependencies

- `config_store.h` from `airgradient-config`
- `go_types.h` for `OperatingMode` enum

## Testability

Same as the reference product pattern. `load_go_settings` and
`save_go_settings` accept a `ConfigStore &` reference. In host tests, provide a
mock `ConfigStore` that returns controlled values. No ESP-IDF dependency in the
settings logic itself — only the NVS backend is platform-specific, and it is
injected.

Test cases:
- Load with empty store returns all defaults
- Load with valid stored values returns stored values
- Load with out-of-range stored values falls back to defaults
- Save with valid settings succeeds and commits
- Save with any invalid field returns false without writing
- Round-trip: save then load returns the same values
