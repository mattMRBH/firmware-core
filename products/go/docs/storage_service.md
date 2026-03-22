# Storage Service

Two-tier storage management for AirGradient Go. Maintains a temporary ring
buffer of sensor measurements for chart rendering (survives deep sleep) and a
persistent binary log of GPS + sensor data for route tracking (survives
power-off). Called synchronously by the orchestrator — no independent task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_storage.h` | `RoutePoint`, `CacheField`, and `StorageService` declarations |
| `products/go/main/go_storage.cpp` | Temporary cache delegation to `PayloadCache`; persistent route file I/O |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `PayloadCache` | `airgradient-payload-cache` (service) | Temporary chart ring buffer |
| `RtcPayloadCacheStorage` | `airgradient-payload-cache` (backend) | RTC memory backend wired at construction time in `main.cpp` |
| `NandStorage` | `airgradient-nand-storage` (HAL) | NAND flash filesystem mount; provides `mount_path()` for POSIX I/O |
| `GpsData` | `airgradient-gps` (`types/gps_types.h`) | GPS position and fix included in each `RoutePoint` |
| `MeasuresAGo` | `airgradient-common` (`measures_types.h`) | Sensor readings in each `RoutePoint` and in the cache |
| `ag_log.h` | `airgradient-common` | Platform-portable logging (no-ops under `TEST_HOST`) |

## Data Tiers

### Temporary (Chart Data)

| Property | Value |
|---|---|
| Backend | `PayloadCache` with `RtcPayloadCacheStorage` |
| Survives deep sleep? | Yes (RTC memory) |
| Survives power-off? | No |
| Semantics | Ring buffer — oldest entry overwritten when full |
| Stored type | `MeasuresAGo` (`CONFIG_PAYLOAD_CACHE_TYPE_AGO`) |
| Capacity | `PAYLOAD_CACHE_MAX_SIZE` (default 16, configurable via Kconfig) |

The cache delegates entirely to `PayloadCache`. `StorageService` adds no
extra logic — `cache_measurement()` calls `push()`,
`backup_cache()`/`restore_cache()` call `backup()`/`restore()`.

### Persistent (Route Data)

| Property | Value |
|---|---|
| Backend | `NandStorage` FATFS via POSIX file I/O |
| Survives deep sleep? | Yes |
| Survives power-off? | Yes |
| File format | Sequential `RoutePoint` structs, no header or framing |
| Directory | `<mount_path>/routes/` |
| File naming | `route_NNNNN.bin` (zero-padded 5-digit session ID, e.g. `route_12345.bin`) |
| One file per | Tracking session identified by a 5-digit random session ID (10000–99999) |

## RoutePoint Struct

```cpp
struct RoutePoint {
    time_t        timestamp; // System time (synced from GPS via settimeofday)
    GpsData       gps;       // Position, altitude, fix type, DOP, satellite count
    MeasuresAGo sensors;   // Primary sensor readings at this point
};
```

Each route file is a flat sequence of `RoutePoint` structs with no header:

```
[RoutePoint][RoutePoint][RoutePoint]...
```

Fixed struct size allows O(1) seeks to the Nth point during off-device
processing.

## CacheField Enum

Used with `read_cached_field()` to identify which measurement field to
extract from the cache history.

| Enumerator | Source field | Unit |
|---|---|---|
| `Temperature` | `MeasuresAGo::temp_hum_a.temperature` | °C |
| `Humidity` | `MeasuresAGo::temp_hum_a.humidity` | % |
| `PM01` | `MeasuresAGo::pm_a.pm_01` (atmospheric) | µg/m³ |
| `PM25` | `MeasuresAGo::pm_a.pm_25` (atmospheric) | µg/m³ |
| `PM10` | `MeasuresAGo::pm_a.pm_10` (atmospheric) | µg/m³ |
| `CO2` | `MeasuresAGo::co2.co2` (cast to float) | ppm |
| `TvocIndex` | `MeasuresAGo::tvoc_nox.tvoc_index` (cast to float) | index |
| `NoxIndex` | `MeasuresAGo::tvoc_nox.nox_index` (cast to float) | index |

Invalid readings (per each field's own `is_*_valid()` method) are written as
the corresponding `MeasuresInvalid` sentinel so the caller can identify and
skip them during rendering.

## API Reference

### Initialization

| Method | Description |
|---|---|
| `init()` | Mounts NAND via `NandStorage::init()`. Returns `false` if NAND mount fails; temporary cache still works independently. |

**Important**: Call `restore_cache()` **before** `init()` after a deep sleep
wake if the previous chart data should be recovered.

### Temporary Cache Operations

| Method | Description |
|---|---|
| `cache_measurement(m)` | Push `MeasuresAGo` into the ring buffer. Overwrites the oldest entry when full. |
| `read_cached_field(field, out, max_count)` | Fill `out[]` with the history of one field across all cached entries, oldest-first. Returns the number of values written. |
| `cached_count()` | Number of measurements currently in the buffer. |
| `backup_cache()` | Persist the ring buffer to RTC memory. Call before deep sleep. |
| `restore_cache()` | Reload the ring buffer from RTC memory. Call after deep sleep wake. |

**Usage example:**

```cpp
float buf[PAYLOAD_CACHE_MAX_SIZE];
uint16_t n = storage.read_cached_field(CacheField::CO2, buf, PAYLOAD_CACHE_MAX_SIZE);
// buf[0..n-1] holds CO2 history oldest-first.
// Values equal to MeasuresInvalid::CO2 (-1) indicate missing readings.
```

### Persistent Route Operations

| Method | Description |
|---|---|
| `start_route(session_id)` | Open or resume a route file for the given session ID. New session → write mode; existing file → append mode with point count restored from file size. Returns `false` if NAND is not mounted or file cannot be opened. |
| `append_route_point(point)` | Write one `RoutePoint` via `fwrite`. Returns `false` if no route is active or write fails. |
| `end_route()` | `fflush` + `fsync` + `fclose`. Resets session ID and point count. Safe to call when no route is active (no-op). |
| `is_route_active()` | Returns `true` while a route file is open. |
| `current_route_point_count()` | Total points written in the current session (includes points from previous boots when resuming). Returns 0 when no route is active. |

## Session ID

The tracking session ID is a 5-digit random integer in the range 10000–99999.
It is generated by the orchestrator when a new tracking session starts and
stored in `RtcAppState::tracking_session_id` so it survives deep sleep.

```cpp
// In go_types.h
struct RtcAppState {
    // ...
    bool     tracking_active     = false;
    uint32_t tracking_session_id = 0; // 0 = no active session
};
```

### Generation algorithm

The orchestrator generates a new ID each time the user starts a fresh tracking
session (i.e. not resuming from sleep). The algorithm uses the hardware RNG
(`esp_random()`) to produce a value in the range 10000–99999 — always 5
digits, never zero (which is the "no active session" sentinel):

```cpp
// Pseudo-code — lives in the orchestrator, not in StorageService.
static constexpr uint32_t SESSION_ID_MIN  = 10000;
static constexpr uint32_t SESSION_ID_SPAN = 90000; // 99999 - 10000 + 1

uint32_t generate_tracking_session_id() {
    return (esp_random() % SESSION_ID_SPAN) + SESSION_ID_MIN;
}
```

The ID is stored immediately in `RtcAppState::tracking_session_id` before
calling `start_route()`, so it is available to the resume path after any
subsequent deep sleep.

`StorageService` receives the ID from the orchestrator via `start_route(session_id)` and
does not generate or persist it internally.

## Deep Sleep and Route Continuity

Deep sleep reboots the CPU — open file handles are lost. Before sleeping the
orchestrator calls `end_route()` to flush and close the file. On wake, it
calls `start_route(rtc_state.tracking_session_id)` with the persisted ID:
`start_route()` detects the existing file, opens it in append mode, and
restores `_current_point_count` from the file size. The session continues
seamlessly as a single file.

```
New session:
  orchestrator generates session_id = 42731
  → start_route(42731)  → creates route_42731.bin (write mode, 0 points)
  → append × N
  → end_route()         → flushes, closes (N points on disk)

Sleep cycle:
  → end_route()         → flushes, closes (N points on disk)
  [deep sleep]
  → start_route(42731)  → finds route_42731.bin, opens in append mode
                          _current_point_count = N (from file size)
  → append × M
  → end_route()         → flushes, closes (N+M points total)
```

## Orchestrator Integration

### On `SensorDataReady` event

```cpp
const MeasuresAGo &basic = event.sensor_data;
storage.cache_measurement(basic);
if (behavior == Behavior::Tracking && storage.is_route_active()) {
    RoutePoint point;
    point.timestamp = get_current_system_time();
    point.gps       = gps_service.get_latest_fix();
    point.sensors   = basic;
    storage.append_route_point(point);
}
```

### On `UserStartTracking` event

```cpp
// Orchestrator generates a new session ID and persists it in RtcAppState.
const uint32_t session_id = generate_tracking_session_id(); // 10000–99999
rtc_state.tracking_session_id = session_id;
rtc_state.tracking_active = true;
if (!storage.start_route(session_id)) {
    // NAND unavailable — show error indicator on display
}
```

### On `UserStopTracking` event

```cpp
storage.end_route();
rtc_state.tracking_active = false;
rtc_state.tracking_session_id = 0;
```

### Before deep sleep

```cpp
storage.backup_cache();
if (storage.is_route_active()) {
    storage.end_route(); // file handles do not survive deep sleep reboot
}
// rtc_state.tracking_active and tracking_session_id remain set if tracking
```

### After deep sleep wake

```cpp
storage.restore_cache(); // before init()
storage.init();          // re-mount NAND
if (rtc_state.tracking_active) {
    storage.start_route(rtc_state.tracking_session_id); // resumes existing file
}
```

## NAND Mount Failure

If `init()` returns `false`:
- `start_route()` returns `false`; route logging is unavailable.
- Temporary cache (`cache_measurement`, `read_cached_field`, etc.) still works
  independently via RTC memory.
- The orchestrator should surface the NAND failure as a status indicator on
  the display.

## Wear and File Size

At one `RoutePoint` per 60 seconds:

| Duration | Points | Approximate size |
|---|---|---|
| 1 hour | 60 | ~12 KB |
| 8 hours | 480 | ~96 KB |
| 1 day | 1440 | ~288 KB |

For a 128 MB NAND with typical wear leveling this is entirely manageable.
`fflush` + `fsync` are called only in `end_route()`. Periodic mid-route
`fsync` (every N points) is a future tuning option if power-loss resilience
becomes a requirement.

## Testability

- **PayloadCache tier**: Inject a `MockPayloadCacheStorage` into `PayloadCache`
  (see `airgradient-payload-cache` test infrastructure). `StorageService`
  delegates directly, so the existing cache tests cover `push`/`peek` behavior.
- **Route I/O tier**: Inject a mock `NandStorage` that returns a temporary
  directory path. POSIX `fopen`/`fwrite`/`fread`/`fclose` run natively on the
  host — no ESP-IDF dependency in the route I/O path.
- **`TEST_HOST` guard**: Logging uses `ag_log.h` which expands to silent no-ops
  under `TEST_HOST`, so `go_storage.cpp` compiles cleanly for native host tests.
