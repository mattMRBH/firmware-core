/**
 * AirGradient Go — GPS Service
 *
 * Runs as an independent RTOS task that calls GpsDriver::read() in a loop,
 * updates the latest fix, syncs the system clock on the first valid timestamp,
 * and posts GpsFixUpdate events to the orchestrator event queue at the
 * configured interval.
 *
 * NMEA parsing and serial I/O are fully delegated to GpsDriver.  This service
 * only orchestrates the task lifecycle and event posting.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "gps/gps_driver.h"
#include "gps/gps_types.h"
#include "rtos.h"

#include <cstdint>

class GpsService {
public:
  struct Config {
    int baud_rate = 115200;
    int posting_interval_ms = 5000; // from GoSettings::gps_interval_seconds
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 3; // must be below display worker (4)
  };

  /// Construct the service.  Does not start the task.
  ///
  /// @param driver      GPS driver (GpsDriver).  Must outlive this service.
  /// @param event_queue  Orchestrator event queue.  Must be created before
  ///                     start() is called.
  /// @param config       Runtime configuration (baud rate, interval, ...).
  GpsService(GpsDriver &driver, RtosQueueHandle event_queue, const Config &config);
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
  GpsDriver &_driver;
  RtosQueueHandle _event_queue;
  Config _config;

  GpsData _latest_fix; // protected by _mutex
  volatile bool _running = false;
  RtosTaskHandle _task_handle = nullptr;
  mutable RtosMutex _mutex;
  RtosBinarySemaphore _done_sem; // signalled by task before self-delete
  bool _clock_synced = false;

  static void task_entry(void *arg); // RTOS task entry point
  void run();                        // actual task loop

  void update_latest_fix(const GpsData &data);    // writes under mutex
  void post_fix_event();                          // post GpsData to event queue
  void sync_system_clock(const GpsTimestamp &ts); // set ESP32 RTC from GPS time
};

/// Synchronous one-shot GPS read.  Blocks until a valid fix is parsed or
/// timeout_ms expires.  For use in the fast-path timer-wake boot path only —
/// does not require a running RTOS task.
///
/// @param driver    GPS driver; begin() / end() are called internally.
/// @param baud_rate  Baud rate passed to GpsDriver::begin().
/// @param timeout_ms Maximum time to wait for a valid fix, in milliseconds.
/// @return           Latest GpsData; check is_fix_valid(data.fix) for validity.
GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms);
