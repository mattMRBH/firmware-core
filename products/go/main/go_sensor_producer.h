/**
 * AirGradient Go — Sensor Producer
 *
 * Wraps the shared SensorManager in an independent FreeRTOS task.  The
 * orchestrator signals it (via xTaskNotify) to run a measurement cycle; the
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
#include "services/sensor_manager.h"

#include <cstdint>

// Forward-declare FreeRTOS opaque types to keep FreeRTOS headers out of this
// public header.  The actual definitions come from freertos/queue.h and
// freertos/task.h at compilation time.
struct QueueDefinition;
typedef QueueDefinition *QueueHandle_t;
struct tskTaskControlBlock;
typedef tskTaskControlBlock *TaskHandle_t;

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
  SensorProducer(SensorManager &manager, QueueHandle_t event_queue, const Config &config);

  /// Start the sensor task.  Call once during initialization.
  /// @return true if the FreeRTOS task was created successfully.
  bool start();

  /// Stop the sensor task.  Sets _running = false then calls
  /// vTaskDelete(_task_handle).  Safe because SensorManager holds no mutexes.
  void stop();

  /// Trigger one measurement cycle with the given iteration count.
  /// Non-blocking: returns immediately after signalling the task via
  /// xTaskNotify(eSetValueWithOverwrite).
  /// @param iterations Number of averaging iterations (minimum 1 enforced
  ///                   inside the task).
  void request_measurement(uint8_t iterations);

private:
  SensorManager &_manager;
  QueueHandle_t _event_queue;
  Config _config;

  volatile bool _running = false;
  TaskHandle_t _task_handle = nullptr;

  static void task_entry(void *arg); ///< FreeRTOS task entry point
  void run();                        ///< Actual task loop
};
