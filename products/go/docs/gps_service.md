# GPS Service

Independent FreeRTOS task that reads GPS data for AirGradient Go. Delegates
all NMEA parsing and serial I/O to the shared `airgradient-gps` component
(`GpsSensor` / `NmeaGps`). The service orchestrates the task lifecycle,
maintains a mutex-protected latest fix, syncs the ESP32 system clock on the
first valid GPS timestamp, and posts `GpsFixUpdate` events to the orchestrator
queue at the configured interval.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_gps.h` | `GpsService` class declaration, `Config` struct, `gps_read_once()` free function |
| `products/go/main/go_gps.cpp` | Task loop, event posting, clock sync, one-shot read implementation |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `GpsSensor` | `airgradient-gps` (`hal/gps_sensor.h`) | Abstract GPS driver interface; injected at construction |
| `NmeaGps` | `airgradient-gps` (`drivers/nmea_gps/nmea_gps.h`) | Concrete driver — wraps `AirgradientSerial`, parses GGA/RMC/GSA sentences via `libnmea-esp32` |
| `GpsData`, `GpsTimestamp`, `GpsFix` | `airgradient-gps` (`types/gps_types.h`) | Canonical GPS data types; used in events and `get_latest_fix()` |
| `RTOS` | `airgradient-common` | `delay_ms()`, `get_time_ms()` — platform-independent timing |
| `go_events.h` | product | `Event`, `EventType::GpsFixUpdate` |
| FreeRTOS task / semaphore / queue | ESP-IDF | Task creation, mutex for `_latest_fix`, done semaphore for clean shutdown, queue send |

## Configuration

`GpsService::Config` fields — hardware-specific values come from `board_config.h`;
interval is derived from `GoSettings::gps_interval_seconds * 1000`.

| Field | Default | Notes |
|---|---|---|
| `baud_rate` | `9600` | GPS module baud rate; hardware-specific, not a user setting |
| `posting_interval_ms` | `5000` | How often to post `GpsFixUpdate` to the event queue; set from `GoSettings::gps_interval_seconds` |
| `task_stack_size` | `4096` | FreeRTOS task stack in bytes; tune at integration time |
| `task_priority` | `5` | Below input task; above idle |

## Usage

```cpp
#include "go_gps.h"
#include "drivers/nmea_gps/nmea_gps.h"
#include "board_config.h"

// Serial transport and driver must outlive the service.
UartSerial gps_uart(BOARD_GPS_UART_PORT, BOARD_GPS_TX_PIN, BOARD_GPS_RX_PIN);
NmeaGps    nmea_gps(gps_uart);

// Build config from settings.
GpsService::Config cfg{};
cfg.posting_interval_ms = settings.gps_interval_seconds * 1000;

GpsService gps_svc(nmea_gps, event_queue, cfg);
gps_svc.start();

// Orchestrator can read the latest fix at any time.
GpsData fix = gps_svc.get_latest_fix();
if (is_fix_valid(fix.fix)) {
    // use fix.position, fix.altitude_m, fix.fix, fix.timestamp
}

// Update interval when settings change.
gps_svc.set_posting_interval_ms(new_interval_ms);

// Clean shutdown before deep sleep.
gps_svc.stop();
```

## Event Output

The service posts `EventType::GpsFixUpdate` to the orchestrator queue at the
configured interval, but only when `GpsSensor::has_valid_fix()` is true. If
no valid fix is available at posting time, the event is skipped; `last_post_ms`
is still updated so the next attempt happens one full interval later.

```cpp
// Event union member (go_events.h):
GpsData gps_data;  // position, altitude_m, fix (type/DOP/sats), timestamp
```

The orchestrator can also call `GpsService::get_latest_fix()` directly at any
time (e.g. when logging a route point after a sensor measurement), independent
of the posting interval.

## Internal Architecture

### Task Loop

```
GpsService::run():
  Create _done_sem (binary semaphore for stop() join)
  GpsSensor::begin(baud_rate)
  last_post_ms = 0

  while _running:
    if GpsSensor::read():                         // drains serial buffer
      data = GpsSensor::get_data()
      update _latest_fix under mutex
      if !_clock_synced and is_gps_timestamp_valid(data.timestamp):
        sync_system_clock(data.timestamp)         // settimeofday(), #ifndef TEST_HOST
        _clock_synced = true

    now_ms = RTOS::get_time_ms()
    if now_ms - last_post_ms >= posting_interval_ms:
      if GpsSensor::has_valid_fix():
        post_fix_event()                          // xQueueSend, non-blocking
      last_post_ms = now_ms

    RTOS::delay_ms(10)                            // yield

  GpsSensor::end()
  xSemaphoreGive(_done_sem)                       // signal stop()
```

### System Clock Sync

On the first call to `GpsSensor::read()` that returns a sentence with a valid
`GpsTimestamp`, the service converts the UTC date/time fields to a POSIX epoch
via `mktime()` and calls `settimeofday()`. A `_clock_synced` flag prevents
repeated syncing.

On ESP-IDF, the default timezone is UTC, so `mktime()` yields the correct
POSIX epoch without any timezone adjustment.

This path is entirely wrapped in `#ifndef TEST_HOST`.

### One-Shot Read (`gps_read_once`)

For the fast-path timer-wake boot path (§7.4 in `ARCHITECTURE.md`), the
orchestrator does not start the full GPS task. Instead it calls:

```cpp
GpsData gps_read_once(GpsSensor &gps, int baud_rate, uint32_t timeout_ms);
```

This function calls `GpsSensor::begin()`, polls `GpsSensor::read()` every
10 ms until `has_valid_fix()` returns true or `timeout_ms` elapses, captures
`get_data()`, calls `GpsSensor::end()`, and returns the result. Callers should
check `is_fix_valid(data.fix)` on the return value.

## Thread Safety

`_latest_fix` is written by the GPS task and read by the orchestrator thread
via `get_latest_fix()`. Access is protected by a FreeRTOS mutex (`_mutex`):

- **GPS task**: `update_latest_fix()` takes the mutex, copies `GpsData` in,
  releases immediately.
- **Orchestrator**: `get_latest_fix()` takes the mutex, copies `GpsData` out,
  releases immediately.

Both holders keep the mutex for a single struct copy — negligible contention.

## Deep Sleep

Before entering deep sleep, the orchestrator calls `stop()`:

1. Sets `_running = false`.
2. Blocks on `_done_sem` (`portMAX_DELAY`) — the GPS task signals this just
   before calling `vTaskDelete(nullptr)`.
3. Deletes `_done_sem`, clears `_task_handle`.

This guarantees the task has fully exited before the system enters sleep. On
wake, the orchestrator calls `start()` again (full wake) or uses
`gps_read_once()` (fast-path timer wake).

The GPS hardware module remains powered during deep sleep and retains its fix.
On task restart, the first NMEA sentences provide an immediate valid fix with
no cold-start delay.

## Testability

FreeRTOS task, semaphore, and queue operations are all guarded by
`#ifndef TEST_HOST`, so `go_gps.cpp` compiles for native host tests.

For host testing:

- Inject a mock `GpsSensor` (via Trompeloeil) that returns controlled
  `GpsData` snapshots from `get_data()` and controlled `has_valid_fix()` /
  `read()` return values.
- Replace the orchestrator queue with a simple test double or inspect the
  `Event` struct directly.
- `gps_read_once()` is testable in isolation by feeding mock serial data
  through a `StubSerial` → `NmeaGps` chain (same pattern used in
  `components/airgradient-gps/tests/nmea_gps.tests.cpp`).

Recommended test cases:

- Task loop posts `GpsFixUpdate` only when `has_valid_fix()` is true.
- Posting is throttled to the configured interval; no duplicate posts within
  the interval.
- `_latest_fix` is updated on every `read()` that returns true, independently
  of the posting interval.
- Clock sync fires on the first valid timestamp and does not fire again.
- `gps_read_once()` returns the fix immediately when the first `read()` call
  produces a valid fix.
- `gps_read_once()` returns an invalid-sentinel `GpsData` when timeout elapses
  before any valid fix.

## Dependencies

- `airgradient-gps` — `GpsSensor` HAL, `NmeaGps` driver, `GpsData` types,
  validation helpers.
- `airgradient-common` — `RTOS` abstraction for timing.
- `go_events.h` / `go_types.h` — event type and payload definitions.
