/**
 * AirGradient Go — Storage Service
 *
 * Two-tier storage management for the Go product:
 *
 * - Temporary (chart data): PayloadCache with RtcPayloadCacheStorage backend.
 *   Survives deep sleep (RTC memory); lost on power-off. Ring-buffer
 *   semantics — oldest entry is overwritten when the buffer is full.
 *
 * - Persistent (route data): POSIX file I/O on an FATFS filesystem provided
 *   by NandStorage. Survives power-off. One binary file per tracking session;
 *   route files are stored at <mount_path>/routes/route_NNNN.bin.
 *
 * No independent task — all methods are called synchronously by the
 * orchestrator.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "measures_types.h"
#include "nand_storage.h"
#include "services/payload_cache.h"
#include "types/gps_types.h"

#include <cstdint>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------------------
// RoutePoint
// ---------------------------------------------------------------------------

/// A single data point in a route log, combining GPS and sensor data with a
/// timestamp.  Written sequentially to a binary route file — no header, no
/// framing.  Fixed size allows O(1) seeks to the Nth point.
struct RoutePoint {
  time_t timestamp; ///< System time at this point (synced from GPS)
  GpsData gps;      ///< GPS position and fix metadata
  Measures sensors; ///< Sensor readings at this point
};

// ---------------------------------------------------------------------------
// StorageService
// ---------------------------------------------------------------------------

class StorageService {
public:
  StorageService(PayloadCache &cache, NandStorage &nand);

  /// Mount NAND and scan for existing route files.  Call once during
  /// initialization.  PayloadCache::restore() should be called separately
  /// before this if RTC data needs to be recovered after deep sleep.
  ///
  /// Returns false if NAND mount fails.  Temporary cache still works
  /// independently of this return value.
  bool init();

  // --- Temporary (chart) operations ---

  /// Push a measurement into the temporary chart cache.
  /// Overwrites the oldest entry when the cache is full.
  void cache_measurement(const Measures &m);

  /// Read a cached measurement by index (0 = oldest).  For chart rendering.
  /// Returns false if index is out of range.
  bool read_cached(uint16_t index, Measures &out) const;

  /// Number of cached measurements currently held in the ring buffer.
  uint16_t cached_count() const;

  /// Backup the cache ring buffer to RTC memory.  Call before deep sleep.
  void backup_cache() const;

  /// Restore the cache ring buffer from RTC memory.  Call after deep sleep
  /// wake, before init().
  void restore_cache();

  // --- Persistent (route) operations ---

  /// Start a new route tracking session.  Creates a new route file at
  /// <mount_path>/routes/route_NNNN.bin.
  /// Returns false if NAND is not mounted, the directory cannot be created,
  /// or the file cannot be opened.
  bool start_route();

  /// Append one data point to the current route file.
  /// Returns false if no route is active or the write fails.
  bool append_route_point(const RoutePoint &point);

  /// Finish the current route tracking session.  Flushes and closes the
  /// route file.  Safe to call when no route is active (no-op).
  void end_route();

  /// Returns true if a route file is currently open.
  bool is_route_active() const;

  /// Number of points written to the currently open route file.
  /// Returns 0 when no route is active.
  uint32_t current_route_point_count() const;

private:
  PayloadCache &_cache;
  NandStorage &_nand;

  FILE *_route_file = nullptr;
  uint16_t _next_route_index = 0;
  uint32_t _current_point_count = 0;

  /// Scan the routes subdirectory for existing route_NNNN.bin files and
  /// return the next available index (highest found + 1, minimum 1).
  uint16_t scan_route_index() const;

  /// Ensure the <mount_path>/routes/ directory exists, creating it if needed.
  /// Returns false if NAND is not mounted or directory creation fails.
  bool ensure_route_dir() const;
};
