/**
 * AirGradient Go — GPS Service
 *
 * Runs as an independent FreeRTOS task that calls GpsSensor::read() in a loop,
 * updates the latest fix, syncs the system clock on the first valid timestamp,
 * and posts GpsFixUpdate events to the orchestrator event queue at the
 * configured interval.
 *
 * NMEA parsing and serial I/O are fully delegated to the airgradient-gps
 * component (GpsSensor / NmeaGps).  This service only orchestrates the task
 * lifecycle and event posting.
 */

#pragma once

#include "hal/gps_sensor.h"
#include "types/gps_types.h"

#include <cstdint>

// Forward-declare FreeRTOS opaque types to keep FreeRTOS headers out of this
// public header.  The actual definitions come from freertos/queue.h and
// freertos/task.h at compilation time.
struct QueueDefinition;
typedef QueueDefinition *QueueHandle_t;
typedef QueueDefinition *SemaphoreHandle_t;

class GpsService {
public:
  struct Config {
    int baud_rate = 9600;
    int posting_interval_ms = 5000; // from GoSettings::gps_interval_seconds
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 5;
  };

  /// Construct the service.  Does not start the task.
  ///
  /// @param gps          GPS sensor driver (NmeaGps or mock).  Must outlive
  ///                     this service.
  /// @param event_queue  Orchestrator event queue.  Must be created before
  ///                     start() is called.
  /// @param config       Runtime configuration (baud rate, interval, …).
  GpsService(GpsSensor &gps, QueueHandle_t event_queue, const Config &config);
  ~GpsService();

  /// Start the GPS reader task.  Call once during initialization.
  /// Returns true if the task was created successfully.
  bool start();

  /// Stop the GPS reader task.  Blocks until the task exits.
  void stop();

  /// Return the most recent parsed fix (thread-safe copy).
  GpsData get_latest_fix() const;

  /// Update the posting interval at runtime (e.g. when settings change).
  void set_posting_interval_ms(int interval_ms);

private:
  GpsSensor &_gps;
  QueueHandle_t _event_queue;
  Config _config;

  GpsData _latest_fix; // protected by _mutex
  volatile bool _running = false;
  void *_task_handle = nullptr; // TaskHandle_t; typed locally in .cpp
  SemaphoreHandle_t _mutex = nullptr;
  SemaphoreHandle_t _done_sem = nullptr; // signalled by task before self-delete
  bool _clock_synced = false;

  static void task_entry(void *arg); // FreeRTOS task entry point
  void run();                        // actual task loop

  void update_latest_fix(const GpsData &data);    // writes under mutex
  void post_fix_event();                          // post GpsData to event queue
  void sync_system_clock(const GpsTimestamp &ts); // set ESP32 RTC from GPS time
};

/// Synchronous one-shot GPS read.  Blocks until a valid fix is parsed or
/// timeout_ms expires.  For use in the fast-path timer-wake boot path only —
/// does not require a running FreeRTOS task.
///
/// @param gps        GPS sensor driver; begin() / end() are called internally.
/// @param baud_rate  Baud rate passed to GpsSensor::begin().
/// @param timeout_ms Maximum time to wait for a valid fix, in milliseconds.
/// @return           Latest GpsData; check is_fix_valid(data.fix) for validity.
GpsData gps_read_once(GpsSensor &gps, int baud_rate, uint32_t timeout_ms);
