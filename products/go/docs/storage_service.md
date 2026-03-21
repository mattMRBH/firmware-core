# Storage Service

Two-tier storage management for AirGradient Go. Maintains a temporary ring
buffer of sensor measurements for chart rendering (survives deep sleep) and a
persistent binary log of GPS + sensor data for route tracking (survives
power-off). Called synchronously by the orchestrator — no independent task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_storage.h` | `RoutePoint` struct and `StorageService` class declaration |
| `products/go/main/go_storage.cpp` | Temporary cache delegation to `PayloadCache`; persistent route file I/O |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `PayloadCache` | `airgradient-payload-cache` (service) | Temporary chart ring buffer |
| `RtcPayloadCacheStorage` | `airgradient-payload-cache` (backend) | RTC memory backend wired at construction time in `main.cpp` |
| `NandStorage` | `airgradient-nand-storage` (HAL) | NAND flash filesystem mount; provides `mount_path()` for POSIX I/O |
| `GpsData` | `airgradient-gps` (`types/gps_types.h`) | GPS position and fix included in each `RoutePoint` |
| `Measures` | `airgradient-common` (`measures_types.h`) | Sensor readings included in each `RoutePoint` and in the cache |

## Data Tiers

### Temporary (Chart Data)

| Property | Value |
|---|---|
| Backend | `PayloadCache` with `RtcPayloadCacheStorage` |
| Survives deep sleep? | Yes (RTC memory) |
| Survives power-off? | No |
| Semantics | Ring buffer — oldest entry overwritten when full |
| Stored type | `Measures` (requires `CONFIG_PAYLOAD_CACHE_TYPE_FULL`, the default) |
| Capacity | `PAYLOAD_CACHE_MAX_SIZE` (default 16, configurable via Kconfig) |

The cache delegates entirely to `PayloadCache`. `StorageService` adds no
extra logic — `cache_measurement()` calls `push()`, `read_cached()` calls
`peek_at_index()`, `backup_cache()`/`restore_cache()` call `backup()`/`restore()`.

### Persistent (Route Data)

| Property | Value |
|---|---|
| Backend | `NandStorage` FATFS via POSIX file I/O |
| Survives deep sleep? | Yes |
| Survives power-off? | Yes |
| File format | Sequential `RoutePoint` structs, no header or framing |
| Directory | `<mount_path>/routes/` |
| File naming | `route_NNNN.bin` (zero-padded 4-digit sequential index) |
| One file per | Tracking session (a `start_route()` → `end_route()` pair) |

## RoutePoint Struct

```cpp
struct RoutePoint {
    time_t timestamp; // System time (synced from GPS via settimeofday)
    GpsData gps;      // Position, altitude, fix type, DOP, satellite count
    Measures sensors; // Full sensor readings at this point
};
```

Each route file is a flat sequence of `RoutePoint` structs with no header:

```
[RoutePoint][RoutePoint][RoutePoint]...
```

Fixed struct size allows O(1) seeks to the Nth point during off-device
processing.

## API Reference

### Initialization

| Method | Description |
|---|---|
| `init()` | Mounts NAND via `NandStorage::init()`, then scans for existing route files. Returns `false` if NAND mount fails; temporary cache still works independently. |

**Important**: Call `restore_cache()` **before** `init()` after a deep sleep
wake if the previous chart data should be recovered.

### Temporary Cache Operations

| Method | Description |
|---|---|
| `cache_measurement(m)` | Push `Measures` into the ring buffer. Overwrites the oldest entry when full. |
| `read_cached(index, out)` | Read by index (0 = oldest). Returns `false` if out of range. |
| `cached_count()` | Number of measurements currently in the buffer. |
| `backup_cache()` | Persist the ring buffer to RTC memory. Call before deep sleep. |
| `restore_cache()` | Reload the ring buffer from RTC memory. Call after deep sleep wake. |

### Persistent Route Operations

| Method | Description |
|---|---|
| `start_route()` | Create a new `route_NNNN.bin` file. Returns `false` if NAND is not mounted or file creation fails. |
| `append_route_point(point)` | Write one `RoutePoint` via `fwrite`. Returns `false` if no route is active or write fails. |
| `end_route()` | `fflush` + `fsync` + `fclose`. Safe to call when no route is active (no-op). Increments the route index. |
| `is_route_active()` | Returns `true` while a route file is open. |
| `current_route_point_count()` | Number of points written to the current route. Returns 0 when no route is active. |

## Route File Indexing

`_next_route_index` is initialized by `scan_route_index()` at `init()` time:

1. Opens the `<mount_path>/routes/` directory.
2. Scans all `route_N.bin` files (any digit count).
3. Returns `max_found_index + 1` (minimum 1 if the directory is empty or
   does not exist).

`end_route()` increments `_next_route_index` after closing the file, so each
subsequent `start_route()` creates a new file.

**Example sequence on a fresh NAND:**

```
init()        → _next_route_index = 1
start_route() → opens route_0001.bin
append × N
end_route()   → closes route_0001.bin, _next_route_index = 2
start_route() → opens route_0002.bin
...
```

**Example sequence after reboot with existing files (route_0001..route_0003):**

```
init()        → scan finds max=3, _next_route_index = 4
start_route() → opens route_0004.bin
```

## Orchestrator Integration

### On `SensorDataReady` event

```cpp
storage.cache_measurement(measures);
if (behavior == Behavior::Tracking && storage.is_route_active()) {
    RoutePoint point;
    point.timestamp = get_current_system_time(); // set by GPS via settimeofday
    point.gps       = gps_service.get_latest_fix();
    point.sensors   = measures;
    storage.append_route_point(point);
}
```

### On `UserStartTracking` event

```cpp
if (!storage.start_route()) {
    // NAND unavailable — show error indicator on display
}
```

### On `UserStopTracking` event

```cpp
storage.end_route();
```

### Before deep sleep

```cpp
storage.backup_cache();
if (storage.is_route_active()) {
    storage.end_route(); // file handles do not survive deep sleep reboot
}
```

### After deep sleep wake

```cpp
storage.restore_cache(); // before init()
storage.init();          // re-mount NAND, rescan route index
if (was_tracking) {
    storage.start_route(); // new file for this wake cycle (Option A)
}
```

## Deep Sleep and Route Continuity

Deep sleep reboots the CPU — open file handles are lost. The current
implementation uses **Option A**: each wake cycle creates a new route file.
A "tracking session" across multiple sleep cycles becomes a series of files
sharing the same timestamp range. Off-device tooling can merge them.

Option B (reopen the last file in append mode) would require storing the
current route filename in RTC memory and is deferred.

## NAND Mount Failure

If `init()` returns `false`:
- `start_route()` returns `false`; route logging is unavailable.
- Temporary cache (`cache_measurement`, `read_cached`, etc.) still works
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
  (see `airgradient-payload-cache` test infrastructure). The `StorageService`
  delegates directly, so the existing cache tests cover the behavior.
- **Route I/O tier**: Inject a mock `NandStorage` that returns a temporary
  directory path (e.g. from `std::tmpnam` or a test fixture). POSIX
  `fopen`/`fwrite`/`fread`/`fclose`/`opendir`/`readdir` run natively on the
  host — no ESP-IDF dependency in the route I/O path.
- **`scan_route_index()`**: Create numbered files in a temp dir, verify the
  scanner returns the expected next index.
- **`TEST_HOST` guard**: `esp_log.h` is stubbed out in `go_storage.cpp` under
  `#ifdef TEST_HOST` so the file compiles cleanly for native host tests.
