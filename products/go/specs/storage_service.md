# Storage Service — Implementation Spec

Product-specific storage service for AirGradient Go. Manages two tiers of data
storage: temporary chart data in RTC memory and persistent route logs on NAND
flash. Called synchronously by the orchestrator — no independent task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_storage.h` | `StorageService` class declaration, `RoutePoint` struct |
| `products/go/main/go_storage.cpp` | Implementation: temporary cache operations, route file I/O |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `PayloadCache` | `airgradient-payload-cache` (service) | Temporary chart ring buffer |
| `RtcPayloadCacheStorage` | `airgradient-payload-cache` (backend) | RTC memory backend for PayloadCache |
| `NandStorage` | `airgradient-nand-storage` (HAL) | NAND flash filesystem mount |
| `go_types.h` | product | `GpsData` struct |
| `measures_types.h` | `airgradient-common` | `Measures` struct |

## Data Tiers

### Temporary (Chart Data)

**Purpose**: Keep the last N measurements for display chart rendering.

- Backend: `PayloadCache` with `RtcPayloadCacheStorage`
- Survives deep sleep (RTC memory)
- Lost on power-off / shutdown
- Ring buffer: overwrites oldest when full
- Stores `Measures` (or `MeasuresAGo` via Kconfig)
- Size: `PAYLOAD_CACHE_MAX_SIZE` (default 16, configurable via Kconfig)

This reuses the existing shared component as-is. No product-specific logic
needed beyond wiring.

### Persistent (Route Data)

**Purpose**: Log GPS + sensor data over time for route tracking.

- Backend: NAND flash via `NandStorage` (FATFS)
- Survives power-off
- POSIX file I/O (`fopen`/`fwrite`/`fread`/`fclose`)
- One file per tracking session (route)

## RoutePoint Struct

A single data point in a route log, combining GPS and sensor data with a
timestamp:

```cpp
struct RoutePoint {
    time_t timestamp;       // system time (synced from GPS)
    GpsData gps;            // position at this point
    Measures sensors;       // sensor readings at this point
};
```

## Route File Format

Binary struct log — each route file is a sequence of `RoutePoint` structs
written sequentially:

```
[RoutePoint][RoutePoint][RoutePoint]...
```

**Rationale**: Binary is compact, fast to write (single `fwrite`), and trivial
to read back. No parsing overhead. The struct is fixed-size, so seeking to the
Nth point is O(1). No need for CSV or JSON complexity on the device; conversion
to human-readable formats happens off-device.

**File naming**: `route_NNNN.bin` where NNNN is a zero-padded sequential index.
The service scans for the highest existing index at mount time and increments.

**Directory**: `<mount_path>/routes/` (e.g. `/nand/routes/route_0001.bin`).

## Class Design

```cpp
#pragma once

#include "go_types.h"
#include "measures_types.h"
#include "nand_storage.h"
#include "payload_cache.h"

#include <cstdio>
#include <cstdint>
#include <ctime>

struct RoutePoint {
    time_t timestamp;
    GpsData gps;
    Measures sensors;
};

class StorageService {
  public:
    StorageService(PayloadCache &cache, NandStorage &nand);

    /// Initialize NAND mount and scan for existing route files.
    /// PayloadCache::restore() should be called separately before this
    /// if RTC data needs to be recovered after deep sleep.
    bool init();

    // --- Temporary (chart) operations ---

    /// Push a measurement to the temporary chart cache.
    void cache_measurement(const Measures &m);

    /// Read a cached measurement by index (0 = oldest). For chart rendering.
    bool read_cached(uint16_t index, Measures &out) const;

    /// Number of cached measurements currently available.
    uint16_t cached_count() const;

    /// Backup cache to RTC memory (call before deep sleep).
    void backup_cache() const;

    /// Restore cache from RTC memory (call after deep sleep wake).
    void restore_cache();

    // --- Persistent (route) operations ---

    /// Start a new route tracking session. Creates a new route file.
    /// Returns false if NAND is not mounted or file creation fails.
    bool start_route();

    /// Append a data point to the current route.
    /// Returns false if no route is active or write fails.
    bool append_route_point(const RoutePoint &point);

    /// Finish the current route tracking session. Closes the file.
    void end_route();

    /// Returns true if a route is currently being recorded.
    bool is_route_active() const;

    /// Get the number of points in the current route.
    uint32_t current_route_point_count() const;

    // --- Route retrieval (future) ---
    // uint16_t get_route_count() const;
    // bool read_route_point(uint16_t route_index, uint32_t point_index,
    //                       RoutePoint &out) const;
    // bool delete_route(uint16_t route_index);

  private:
    PayloadCache &_cache;
    NandStorage &_nand;

    FILE *_route_file = nullptr;
    uint16_t _next_route_index = 0;
    uint32_t _current_point_count = 0;

    uint16_t scan_route_index() const;
    bool ensure_route_dir() const;
};
```

## Orchestrator Integration

The orchestrator calls StorageService synchronously. Typical flows:

### On SensorDataReady event:
```
storage.cache_measurement(measures);
if (behavior == Tracking && storage.is_route_active()):
    RoutePoint point;
    point.timestamp = current_system_time();
    point.gps = gps_service.get_latest_fix();
    point.sensors = measures;
    storage.append_route_point(point);
```

### On UserStartTracking:
```
storage.start_route();
```

### On UserStopTracking:
```
storage.end_route();
```

### Before deep sleep:
```
storage.backup_cache();
if (storage.is_route_active()):
    // fsync the route file to flush writes
    // file stays open across sleep? No — deep sleep reboots.
    // Must close and reopen on wake.
    storage.end_route();
```

### After deep sleep wake (if tracking was active):
```
storage.restore_cache();
storage.init();  // re-mount NAND
// Route continuity: start a new route file (or append to previous — see below)
storage.start_route();
```

## Deep Sleep and Route Continuity

Deep sleep reboots the CPU. Open file handles are lost. Two options for
route continuity across sleep:

**Option A — New file per wake cycle**: Each wake creates a new `route_NNNN.bin`.
A "tracking session" becomes a series of small files. Off-device tooling merges
them by timestamp. Simple implementation. Downside: many small files.

**Option B — Reopen and append**: On wake, reopen the last route file in append
mode. Requires storing the current route filename in RTC memory. More complex
but produces one file per tracking session.

**Recommendation**: Start with Option A (simplest). Evaluate file count in
practice. Switch to Option B if needed.

## Flash Wear Considerations

- NAND flash + FATFS handles wear leveling at the filesystem level
- Route logging is append-only (no random writes)
- Each route point is ~200 bytes. At 1 point per 60 seconds, a full day of
  tracking is ~288 KB. Manageable for typical NAND flash sizes (128MB+)
- `fclose()` or `fsync()` should be called periodically (e.g. every N points)
  to avoid data loss on unexpected power loss. The exact interval is a tuning
  parameter.

## NAND Mount Failure

If NAND initialization fails (`init()` returns false):
- Temporary cache still works (RTC memory, independent of NAND)
- Route logging is unavailable — `start_route()` returns false
- The orchestrator should surface this as a status indicator on display
- Sensor measurement and display continue normally

## Testability

For host testing under `TEST_HOST`:

- **PayloadCache**: Already host-testable. Inject a mock
  `PayloadCacheStorage` or use the existing test infrastructure.
- **Route I/O**: Use a mock `NandStorage` that provides a temp directory path.
  POSIX file operations (`fopen`/`fwrite`/`fread`) work natively on the host.
  Write route points, read them back, verify binary format.
- **RoutePoint**: Plain struct, trivially testable.
- **scan_route_index()**: Create numbered files in a temp dir, verify the
  scanner finds the correct next index.
