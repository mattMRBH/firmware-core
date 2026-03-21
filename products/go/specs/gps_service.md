# GPS Service — Implementation Spec

Product-specific GPS service for AirGradient Go. Runs as an independent
FreeRTOS task that reads NMEA sentences over UART, parses them, and posts
`GpsFixUpdate` events to the orchestrator event queue.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_gps.h` | `GpsService` class declaration |
| `products/go/main/go_gps.cpp` | Task loop, NMEA parsing, event posting |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `AirgradientSerial` | `airgradient-serial` (shared component) | UART read/write to GPS module |
| NMEA parser | third-party (TBD) | Parse NMEA sentences into position/time fields |
| `go_types.h` | product | `GpsData` struct |
| `go_events.h` | product | `Event`, `EventType::GpsFixUpdate` |
| FreeRTOS queue | ESP-IDF / RTOS | `xQueueSend` to orchestrator event queue |

### NMEA Parser Selection

The NMEA parser is a third-party dependency. Candidate: **minmea** — lightweight
C library, no dynamic allocation, parses GGA/RMC/GSA sentences. Small footprint,
suitable for embedded. Final selection TBD at implementation time.

Required NMEA sentences:
- **GGA**: latitude, longitude, altitude, satellite count, fix quality
- **RMC**: latitude, longitude, speed, UTC date+time, fix validity

## Class Design

```cpp
#pragma once

#include "airgradient_serial.h"
#include "go_events.h"
#include "go_types.h"

#include <cstdint>

// Forward declare FreeRTOS types to avoid header dependency
// (actual types come from freertos/queue.h at build time)
struct QueueDefinition;
typedef QueueDefinition *QueueHandle_t;

class GpsService {
  public:
    struct Config {
        int baud_rate             = 9600;
        int posting_interval_ms   = 5000;  // from GoSettings::gps_interval_seconds
        uint16_t task_stack_size  = 4096;
        uint8_t task_priority     = 5;
    };

    GpsService(AirgradientSerial &serial, QueueHandle_t event_queue,
               const Config &config);

    /// Start the GPS reader task. Call once during initialization.
    /// Returns true if the task was created successfully.
    bool start();

    /// Stop the GPS reader task. Blocks until the task exits.
    void stop();

    /// Get the most recent parsed fix (thread-safe copy).
    GpsData get_latest_fix() const;

    /// Update the posting interval at runtime (e.g. when settings change).
    void set_posting_interval_ms(int interval_ms);

  private:
    AirgradientSerial &_serial;
    QueueHandle_t _event_queue;
    Config _config;

    GpsData _latest_fix;               // protected by mutex or atomic flag
    volatile bool _running = false;
    TaskHandle_t _task_handle = nullptr;

    static void task_entry(void *arg);  // FreeRTOS task entry point
    void run();                         // actual task loop

    bool read_and_parse();              // read bytes, accumulate sentence, parse
    void post_fix_event();              // post GpsData to event queue
    void sync_system_clock(time_t utc); // set ESP32 RTC from GPS time
};
```

## Task Loop

```
GpsService::run():
    _serial.begin(_config.baud_rate)
    last_post_time = 0

    while (_running):
        // Non-blocking read: consume all available bytes
        while (_serial.available() > 0):
            byte = _serial.read()
            feed byte to NMEA sentence accumulator

            if complete sentence received:
                parse sentence (GGA or RMC)
                update _latest_fix fields from parsed data

                if RMC with valid date+time and system clock not yet synced:
                    sync_system_clock(parsed_utc_epoch)

        // Throttled posting to event queue
        now = RTOS::get_time_ms()
        if (now - last_post_time >= _config.posting_interval_ms):
            if _latest_fix.fix_valid:
                post_fix_event()
            last_post_time = now

        // Small yield to avoid busy-waiting when no data available
        RTOS::delay_ms(10)
```

### NMEA Sentence Accumulation

NMEA sentences are line-based (`$...*CC\r\n`). The task reads bytes one at a
time from the serial port and accumulates them into a line buffer. When a
complete sentence is detected (newline received), it is handed to the NMEA
parser.

Line buffer size: 128 bytes (longest standard NMEA sentence is ~82 characters).

### Fix Data Update

When a GGA sentence is parsed: update latitude, longitude, altitude, satellites,
fix quality. When an RMC sentence is parsed: update latitude, longitude, speed,
UTC timestamp, fix validity.

Both sentence types contribute to the same `_latest_fix` struct. Fields are
updated incrementally — a GGA updates position fields, an RMC updates speed and
time. The `fix_valid` flag comes from RMC status.

### System Clock Sync

On the first valid RMC sentence with date and time, convert to `time_t` epoch
and set the ESP32 system clock via `settimeofday()` or equivalent. Set a flag
to avoid repeated syncing. Optionally re-sync periodically (e.g. once per hour)
to correct drift.

This is wrapped in `#ifndef TEST_HOST` since `settimeofday` is platform-
specific.

## Thread Safety

`_latest_fix` is accessed by both the GPS task (writer) and the orchestrator
thread via `get_latest_fix()` (reader). Options:

1. **Mutex**: simple, correct, small overhead for infrequent reads
2. **Copy under critical section**: even simpler for a small struct

Prefer a mutex. The GPS task holds it briefly during fix update. The
orchestrator holds it briefly during `get_latest_fix()`.

## Interaction with Orchestrator

The GPS service has two interaction points with the orchestrator:

1. **Event queue**: GPS task posts `GpsFixUpdate` events at the configured
   interval. The orchestrator processes these like any other event.

2. **Direct read**: The orchestrator can call `get_latest_fix()` at any time
   (e.g. when logging a route point after a sensor measurement completes). This
   returns the most recent parsed fix, regardless of the posting interval.

The GPS service does not consume events from the orchestrator. It does not
know about modes or behaviors. The orchestrator decides whether to use GPS data
based on the `gps_enabled` setting — it simply ignores `GpsFixUpdate` events
when GPS is disabled in software.

## Deep Sleep Behavior

Before deep sleep, the orchestrator calls `stop()` to cleanly shut down the
task. On wake, the orchestrator calls `start()` again (or skips it on fast-path
timer wake if GPS data is not needed).

The GPS hardware module stays powered during deep sleep and maintains its fix.
On task restart, the first NMEA sentences from the module provide an immediate
valid fix — no cold start delay.

## Fast-Path Wake (Timer)

During fast-path timer wake (locked, no full event loop), the orchestrator does
not start the GPS task. Instead, if tracking is active and a route point is
needed, the orchestrator can read NMEA directly from the serial port in a
synchronous one-shot fashion:

```
serial.begin(9600)
// read bytes until a valid GGA or RMC sentence is received (with timeout)
// parse and extract GpsData
serial.end()
```

This avoids starting the full task infrastructure for a single data point. A
static helper function in `go_gps.cpp` can provide this:

```cpp
/// Synchronous one-shot GPS read. Blocks until a valid fix is parsed
/// or timeout_ms expires. For use in fast-path boot only.
GpsData gps_read_once(AirgradientSerial &serial, int baud_rate,
                      uint32_t timeout_ms);
```

## Configuration

All configurable values come from `GoSettings`:

| GpsService::Config field | GoSettings source | Notes |
|---|---|---|
| `baud_rate` | hardcoded (board_config.h) | GPS module specific, not a user setting |
| `posting_interval_ms` | `gps_interval_seconds * 1000` | Converted from seconds to ms |
| `task_stack_size` | hardcoded | Tuned at integration time |
| `task_priority` | hardcoded | Below sensor task, above idle |

## Testability

For host testing under `TEST_HOST`:

- Inject a mock `AirgradientSerial` that returns pre-recorded NMEA byte
  sequences
- Replace the FreeRTOS queue with a test double (or test `read_and_parse()`
  and fix construction in isolation without the task loop)
- `gps_read_once()` is testable by feeding mock serial data

The NMEA parsing logic itself (sentence accumulation + parser invocation) can be
unit-tested independently of the task infrastructure by extracting it into pure
functions that take a byte buffer and return a `GpsData`.
