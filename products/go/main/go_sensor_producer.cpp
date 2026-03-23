/**
 * AirGradient Go — Sensor Producer implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_sensor_producer.h"

#include "go_events.h"
#include "rtos.h"

static constexpr const char *TAG = "SensorProducer";

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SensorProducer::SensorProducer(SensorManager &manager, RtosQueueHandle event_queue,
                               const Config &config)
    : _manager(manager), _event_queue(event_queue), _config(config) {}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool SensorProducer::start() {
  _running = true;

  const bool ok =
      RTOS::task_create(task_entry, "sensor_prod", static_cast<uint32_t>(_config.task_stack_size),
                        this, static_cast<uint32_t>(_config.task_priority), &_task_handle);

  if (!ok) {
    _running = false;
    _task_handle = nullptr;
    return false;
  }
  return true;
}

void SensorProducer::stop() {
  _running = false;
  if (_task_handle != nullptr) {
    // Deleting a task that may be blocked inside start_measures() or waiting
    // for a notification is safe because SensorManager holds no mutexes.
    RTOS::task_delete(_task_handle);
    _task_handle = nullptr;
  }
}

void SensorProducer::request_measurement(uint8_t iterations) {
  if (_task_handle != nullptr) {
    RTOS::task_notify_send(_task_handle, static_cast<uint32_t>(iterations));
  }
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

    // Block indefinitely until the orchestrator sends a task notification
    // with the desired iteration count.
    RTOS::task_notify_wait(&iterations, UINT32_MAX);

    // stop() may have set _running = false and sent a notification with
    // value 0 to unblock this wait before calling RTOS::task_delete.
    if (!_running) {
      break;
    }

    // Guard against an accidental zero iteration count.
    if (iterations == 0) {
      iterations = 1;
    }

    // Blocking call: takes (iterations * CONFIG_AVERAGING_ITERATION_INTERVAL_MS).
    // The orchestrator continues processing other events while we block here.
    const Measures measures = _manager.start_measures(static_cast<int>(iterations));

    // Map to MeasuresAGo — select the primary sensor channels only.
    // TODO: Map pressure sensor here, then perhaps change "basic" to something else
    MeasuresAGo basic{};
    basic.temp_hum_a = measures.temp_hum_a;
    basic.pm_a = measures.pm_a;
    basic.co2 = measures.co2;
    basic.tvoc_nox = measures.tvoc_nox;
    basic.power = measures.power;

    // Post result to the orchestrator event queue (non-blocking: drop if full).
    Event event{};
    event.type = EventType::SensorDataReady;
    event.sensor_data = basic;
    RTOS::queue_send(_event_queue, &event, 0);
  }
}
