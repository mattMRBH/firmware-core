/**
 * AirGradient Go — Sensor Producer
 *
 * Wraps the shared SensorManager in an independent RTOS task.  The
 * orchestrator signals it (via task notification) to run a measurement cycle; the
 * task calls SensorManager::start_measures() — which blocks for the full
 * averaging duration — then posts a SensorDataReady event to the orchestrator
 * event queue.
 *
 * AGo sensor wiring (owned by main.cpp wiring layer, not by this class):
 *   sensors.temp_hum     = &sht40_driver
 *   sensors.co2          = &co2_driver       // S8 or Sunlight
 *   sensors.pms_a        = &pms5003_driver
 *   sensors.pms_b        = nullptr
 *   sensors.tvoc_nox     = &sgp41_driver
 *   sensors.o3_no2       = nullptr
 *
 * SensorProducer holds only a SensorManager reference.  It has no knowledge
 * of which sensors are wired — that is the product wiring layer's
 * responsibility.
 */

#pragma once

#include "go_events.h"
#include "measures_types.h"
#include "rtos.h"
#include "services/sensor_manager.h"

#include <cstdint>

class SensorProducer {
public:
  struct Config {
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 5;
  };

  /// Construct the producer.  Does not start the task.
  ///
  /// @param manager      SensorManager to drive.  Must outlive this producer.
  /// @param event_queue  Orchestrator event queue.  Must be created before
  ///                     start() is called.
  /// @param config       Task stack size and priority.
  SensorProducer(SensorManager &manager, RtosQueueHandle event_queue, const Config &config);

  /// Start the sensor task.  Call once during initialization.
  /// @return true if the task was created successfully.
  bool start();

  /// Stop the sensor task.  Sets _running = false then deletes the task.
  /// Safe because SensorManager holds no mutexes.
  void stop();

  /// Trigger one measurement cycle with the given iteration count.
  /// Non-blocking: returns immediately after signalling the task via
  /// RTOS::task_notify_send().
  /// @param iterations Number of averaging iterations (minimum 1 enforced
  ///                   inside the task).
  void request_measurement(uint8_t iterations);

private:
  SensorManager &_manager;
  RtosQueueHandle _event_queue;
  Config _config;

  volatile bool _running = false;
  RtosTaskHandle _task_handle = nullptr;

  static void task_entry(void *arg); ///< RTOS task entry point
  void run();                        ///< Actual task loop
};
