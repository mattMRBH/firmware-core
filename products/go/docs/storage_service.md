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
| `RtcPayloadCacheStorage` | `airgradient-payload-cache` (backend) | RTC memory backend wired at construction time in `GoHardwareBoard::storage()` |
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
`backup_cache()`/`restore_cache()` call `backup()`/`restore()`, and
`clear_cache()` calls `clean()`.

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
    time_t      timestamp;            // System time (synced from GPS via settimeofday)
    GpsData     gps;                  // Position, altitude, fix type, DOP, satellite count
    MeasuresAGo sensors;              // Primary sensor readings at this point
    float       battery_percentage;   // Battery SOC (0–100 %), -1 = invalid
};
```

Each route file is a flat sequence of `RoutePoint` structs with no header:

```text
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

Storage always receives raw `MeasuresAGo` values. Measurement correction is not
applied by `StorageService`, so RTC chart samples and persistent route points
retain the original sensor readings. The orchestrator corrects chart scratch
copies when rendering, while BLE History exports the raw route-point copies and
leaves correction policy to the client.

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

The open API is split into two explicit-intent methods so the resume path and
the fresh-start path cannot be confused. Both methods refuse to proceed when
a route is already active, leaving the existing route untouched.

| Method | Description |
|---|---|
| `create_route(session_id)` | Open a brand-new route file. Refuses if the file already exists (no truncating-open) or if `stat()` fails with anything other than `ENOENT`. On success performs an immediate `fflush + fsync` of the empty file so the directory entry is durable on NAND before the orchestrator tells the user / phone the session started. Marked `[[nodiscard]]`. |
| `resume_route(session_id)` | Reopen an existing route file in append mode after deep-sleep wake. Truncates any torn trailing record (size not aligned to `sizeof(RoutePoint)`) via `ftruncate()` before opening so the next append lands on a clean boundary. Marked `[[nodiscard]]`. |
| `route_file_exists(session_id)` | Cheap `stat()` check used by the orchestrator's session-ID retry loop so collisions never surface to the user as storage errors. Returns `false` when NAND is not mounted. |
| `append_route_point(point)` | Write one `RoutePoint` via `fwrite`. Internally enforces the durability budget below — flushes + fsyncs at most every `CONFIG_TRACKING_FSYNC_INTERVAL_MS`, plus an unconditional sync on the very first post-open append. Returns `false` on `fwrite`, `fflush`, or `fsync` failure. Marked `[[nodiscard]]`. |
| `end_route()` | `fflush` + `fsync` + `fclose` (unconditional). Resets session ID, point count, and the budget anchor. Safe to call when no route is active (no-op). |
| `is_route_active()` | Returns `true` while a route file is open. |
| `current_route_point_count()` | Total points written in the current session (includes points from previous boots when resuming). Returns 0 when no route is active. |
| `clear_routes()` | Deletes all files under `<mount_path>/routes/`. Used by Clear Data and Factory Reset. Returns `true` when all route files are removed. |
| `total_capacity_kb()` | Total FATFS capacity in kilobytes for BLE status reporting. Uses `esp_vfs_fat_info()` on target and `statvfs()` under `TEST_HOST`. |
| `used_kb()` | Used FATFS capacity in kilobytes for BLE status reporting. Uses the same target/host split as `total_capacity_kb()`. |

### Durability Budget

`append_route_point()` forces an `fflush + fsync` to NAND under three
conditions, bounding the worst-case data loss on a power cut to the
configured window regardless of the measurement period:

1. **Empty-file sync** — `create_route()` syncs the newly opened (still empty)
   file before returning success. The session's existence is durable from the
   moment the orchestrator can tell the user / phone "tracking = true". A
   failure of this sync fails the whole `create_route()` call and leaves no
   half-open state.
2. **First-append sync** — both `create_route()` and `resume_route()` initialise
   the budget anchor (`_last_fsync_ms = 0`) so the first successful
   `append_route_point()` after open unconditionally crosses the threshold and
   syncs. This guarantees that the first point of a fresh session, or the
   first point after a deep-sleep resume, lands on NAND regardless of
   cadence.
3. **Steady-state cadence** — subsequent appends sync at most every
   `CONFIG_TRACKING_FSYNC_INTERVAL_MS` (default 30 s).

Any `fflush` or `fsync` failure inside `append_route_point()` surfaces as a
`false` return. The orchestrator logs and ignores the failure, keeping the
session running so subsequent appends can retry; only a manual stop or
deep sleep ends the session.

### Resume-time Torn-Record Truncate

`resume_route()` runs a `stat()` of the file before reopening. If the
returned size is not a clean multiple of `sizeof(RoutePoint)` — i.e. a prior
boot died mid-`fwrite` and FATFS metadata caught up with the partial record —
the helper truncates the file to the nearest record boundary via
`ftruncate()` on a transient `"rb+"` open. The number of dropped bytes is
logged at `WARN`. Without this step, the next append would silently write
after a torn record and corrupt the binary stream.

The implementation uses `ftruncate()` (on an open fd) rather than the
path-based `truncate()` because ESP-IDF FATFS VFS explicitly supports
`ftruncate()`, while `truncate()` is not guaranteed across all VFS
implementations.

### Host-Test Seam

Under `TEST_HOST` only, `StorageService` exposes a `StorageTestSeam` struct
and a `set_test_seam(StorageTestSeam *)` accessor. Installing a seam routes
the internal `fflush` and `fsync` calls through caller-owned counters and
return-code overrides — host tests verify cadence, failure-return
surfacing, and `_last_fsync_ms` update behavior directly via call counts
without depending on libc buffering or FATFS cache semantics. Production
builds compile to direct `fflush` / `fsync` calls with no overhead.

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
session (i.e. not resuming from sleep). It draws 5-digit random integers
from the shared `generate_random_number(5)` helper in `airgradient-common`
(hardware RNG on target builds, `std::rand()` under `TEST_HOST`) and probes
each candidate against `StorageService::route_file_exists()`. On a collision
it retries up to `SESSION_ID_MAX_RETRIES` (5) times before giving up.

At ~1000 stored sessions across the 90,000-slot space the
all-five-collide probability is on the order of `(1000/90000)^5 ≈ 1.7×10⁻¹⁰`,
so retry exhaustion is effectively never observed. When it does happen,
`generate_session_id()` returns 0 and `start_tracking()` treats that as a
storage error — the user sees the standard `"Storage error — can't track"`
snackbar.

The ID is stored immediately in `RtcAppState::tracking_session_id` so it is
available to the resume path after any subsequent deep sleep.

`StorageService` receives the ID from the orchestrator via `create_route()`
(fresh session) or `resume_route()` (after deep sleep) and does not generate
or persist it internally.

## Deep Sleep and Route Continuity

Deep sleep reboots the CPU — open file handles are lost. Before sleeping the
orchestrator calls `end_route()` to flush and close the file. On wake, it
calls `resume_route(rtc_state.tracking_session_id)` with the persisted ID:
`resume_route()` truncates any torn trailing record (see above), opens the
file in append mode, and restores `_current_point_count` from the file size.
The session continues seamlessly as a single file.

```text
New session:
  orchestrator generates session_id = 42731 (with collision retry probe)
  → create_route(42731)  → creates route_42731.bin (empty), fsyncs it,
                            returns true (0 points)
  → append × N           (first append unconditionally fsyncs; further
                            appends fsync at most every
                            CONFIG_TRACKING_FSYNC_INTERVAL_MS)
  → end_route()          → flushes, closes (N points on disk)

Sleep cycle:
  → end_route()          → flushes, closes (N points on disk)
  [deep sleep]
  → resume_route(42731)  → truncates any torn trailing record, opens in
                            append mode, _current_point_count = N
  → append × M
  → end_route()          → flushes, closes (N+M points total)
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
// Orchestrator generates a new session ID with collision retry, then
// opens the file via the explicit-intent create_route().
const uint32_t session_id = generate_session_id(); // retry-on-collision; 0 = exhausted
if (session_id == 0 || !storage.create_route(session_id)) {
    // NAND unmounted, ID exhaustion, or open / empty-file fsync failed —
    // surface "Storage error — can't track" snackbar and BLE notify_tracking_status
    // with tracking=false inline. Do NOT set tracking_active.
    return false;
}
rtc_state.tracking_session_id = session_id;
rtc_state.tracking_active = true;
```

### On `UserStopTracking` event

```cpp
storage.end_route();
rtc_state.tracking_active = false;
rtc_state.tracking_session_id = 0;
```

### On `ClearData` / factory reset

```cpp
storage.clear_cache();
storage.clear_routes();
```

`clear_data()` uses these helpers directly. Factory reset builds on top of the
same storage clear operations before resetting settings and BLE bonds.

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
    if (!storage.resume_route(rtc_state.tracking_session_id)) {
        // Persistent NAND fault. Clear tracking state inline and show
        // "Tracking stopped — storage" snackbar. BLE is not yet up at
        // this point; Read on connect is authoritative.
    }
}
```

## NAND Mount Failure

If `init()` returns `false`:

- `create_route()` / `resume_route()` / `route_file_exists()` all return
  `false` immediately; route logging is unavailable.
- Temporary cache (`cache_measurement`, `read_cached_field`, etc.) still works
  independently via RTC memory.
- The orchestrator surfaces the NAND failure inline via the
  `"Storage error — can't track"` snackbar and a BLE Status notify with
  `tracking=false` whenever the user attempts to start tracking.

## Wear and File Size

At one `RoutePoint` per 60 seconds:

| Duration | Points | Approximate size |
|---|---|---|
| 1 hour | 60 | ~12 KB |
| 8 hours | 480 | ~96 KB |
| 1 day | 1440 | ~288 KB |

For a 128 MB NAND with typical wear leveling this is entirely manageable.

`append_route_point()` enforces the durability budget — at 1 s sample with a
30 s window the steady-state cost is ~2 NAND syncs per minute, plus the
empty-file sync on each `create_route()` and the unconditional first-append
sync. At long sample cadences (period ≥ window) every append syncs anyway
because each one is more than one window after the previous, with no
regression from the pre-spec "sync only in `end_route()`" behavior.

## Testability

- **PayloadCache tier**: Inject a `MockPayloadCacheStorage` into `PayloadCache`
  (see `airgradient-payload-cache` test infrastructure). `StorageService`
  delegates directly, so the existing cache tests cover `push`/`peek` behavior.
- **Route I/O tier**: Inject a mock `NandStorage` that returns a temporary
  directory path. POSIX `fopen`/`fwrite`/`fread`/`fclose` run natively on the
  host — no ESP-IDF dependency in the route I/O path.
- **`TEST_HOST` guard**: Logging uses `ag_log.h` which expands to silent no-ops
  under `TEST_HOST`, so `go_storage.cpp` compiles cleanly for native host tests.
