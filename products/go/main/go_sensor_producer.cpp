/**
 * AirGradient Go — Sensor Producer implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_sensor_producer.h"

// FreeRTOS task notification API (not available in TEST_HOST builds).
#ifndef TEST_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "go_events.h"
#include "rtos.h"

static constexpr const char *TAG = "SensorProducer";

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SensorProducer::SensorProducer(SensorManager &manager, QueueHandle_t event_queue,
                               const Config &config)
    : _manager(manager), _event_queue(event_queue), _config(config) {}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool SensorProducer::start() {
  _running = true;

  // RTOS::task_create stores an opaque void* handle; cast to TaskHandle_t
  // after creation to keep the header free of FreeRTOS includes.
  void *raw_handle = nullptr;
  const bool ok =
      RTOS::task_create(task_entry, "sensor_task", static_cast<uint32_t>(_config.task_stack_size),
                        this, static_cast<uint32_t>(_config.task_priority), &raw_handle);

  _task_handle = static_cast<TaskHandle_t>(raw_handle);

  if (!ok) {
    _running = false;
    _task_handle = nullptr;
    return false;
  }
  return true;
}

void SensorProducer::stop() {
  _running = false;
#ifndef TEST_HOST
  if (_task_handle != nullptr) {
    // vTaskDelete is safe here: SensorManager holds no mutexes, so deleting
    // a task that may be blocked inside start_measures() or waiting for a
    // notification does not risk deadlock.
    vTaskDelete(_task_handle);
    _task_handle = nullptr;
  }
#endif
}

void SensorProducer::request_measurement(uint8_t iterations) {
#ifndef TEST_HOST
  if (_task_handle != nullptr) {
    // eSetValueWithOverwrite: always update the iteration count even if the
    // previous notification has not been consumed yet.
    xTaskNotify(_task_handle, static_cast<uint32_t>(iterations), eSetValueWithOverwrite);
  }
#endif
}

// ---------------------------------------------------------------------------
// Task entry point (static)
// ---------------------------------------------------------------------------

// static
void SensorProducer::task_entry(void *arg) {
  static_cast<SensorProducer *>(arg)->run();
  RTOS::task_delete(nullptr);
}

// ---------------------------------------------------------------------------
// Task loop
// ---------------------------------------------------------------------------

void SensorProducer::run() {
  while (_running) {
    uint32_t iterations = 0;

#ifndef TEST_HOST
    // Block indefinitely until the orchestrator sends a task notification
    // with the desired iteration count.
    xTaskNotifyWait(0, 0, &iterations, portMAX_DELAY);
#endif

    // stop() may have set _running = false and sent a notification with
    // value 0 to unblock this wait before calling vTaskDelete.
    if (!_running) {
      break;
    }

    // Guard against an accidental zero iteration count.
    if (iterations == 0) {
      iterations = 1;
    }

    // Blocking call: takes (iterations * CONFIG_AVERAGING_ITERATION_INTERVAL_MS).
    // The orchestrator continues processing other events while we block here.
    Measures measures = _manager.start_measures(static_cast<int>(iterations));

    // Post result to the orchestrator event queue (non-blocking: drop if full).
    Event event{};
    event.type = EventType::SensorDataReady;
    event.sensor_data = measures;
    RTOS::queue_send(_event_queue, &event, 0);
  }
}
