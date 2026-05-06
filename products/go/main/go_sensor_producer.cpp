/**
 * AirGradient Go — Sensor Producer implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_sensor_producer.h"

#include "ag_log.h"
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
    // Send a zero-value notification to unblock task_notify_wait so the task
    // can check _running and exit the loop cleanly when possible.
    RTOS::task_notify_send(_task_handle, 0);
    RTOS::delay_ms(10);
    // Forcefully delete — safe because SensorManager holds no mutexes.
    // The task entry function blocks forever after run() returns (instead of
    // self-deleting) to avoid a double-delete race with this call.
    RTOS::task_delete(_task_handle);
    _task_handle = nullptr;
  }
}

void SensorProducer::request_measurement(uint8_t iterations, SensorGroup groups) {
  if (_task_handle != nullptr) {
    // Pack iterations (bits 0-7) and group mask (bits 8-15) into one notify value
    uint32_t value = (static_cast<uint32_t>(groups) << 8) | iterations;
    RTOS::task_notify_send(_task_handle, value);
  }
}

void SensorProducer::request_co2_calibration() {
  if (_task_handle != nullptr) {
    RTOS::task_notify_send(_task_handle, NOTIFY_CALIBRATION);
  }
}

void SensorProducer::request_prepare() {
  if (_task_handle != nullptr) {
    RTOS::task_notify_send(_task_handle, NOTIFY_PREPARE);
  }
}

// ---------------------------------------------------------------------------
// Task entry point (static)
// ---------------------------------------------------------------------------

// static
void SensorProducer::task_entry(void *arg) {
  static_cast<SensorProducer *>(arg)->run();
  // Block indefinitely — stop() will delete this task externally.
  // Must not return: FreeRTOS task functions that return cause undefined
  // behavior.  Self-delete is intentionally avoided here because stop()
  // performs a forced task_delete, and both paths racing would cause a
  // double-delete crash.
  while (true) {
    RTOS::delay_ms(60000);
  }
}

// ---------------------------------------------------------------------------
// Task loop
// ---------------------------------------------------------------------------

void SensorProducer::run() {
  // Warm up TVOC/NOx (SGP41 conditioning) and PM sensor (SPS30 fan/laser)
  // before serving the first measurement request. Blocks this task for
  // ~CONFIG_SENSOR_WARMUP_DURATION_MS;
  // The orchestrator's initial request_measurement(1, All)
  // from Orchestrator::init() latches into the FreeRTOS task notification
  // slot and is honoured as soon as this returns.
  AG_LOGI(TAG, "warming up sensors before first measurement");
  _manager.warmup();
  AG_LOGI(TAG, "warmup complete");

  while (_running) {
    uint32_t notify_value = 0;

    // Block indefinitely until the orchestrator sends a task notification.
    // The value encodes the request type: measurement (iterations + groups),
    // NOTIFY_CALIBRATION, or NOTIFY_PREPARE.
    RTOS::task_notify_wait(&notify_value, UINT32_MAX);

    // stop() may have set _running = false and sent a notification with
    // value 0 to unblock this wait before calling RTOS::task_delete.
    if (!_running) {
      break;
    }

    if (notify_value == NOTIFY_CALIBRATION) {
      // Blocking: polls sensor until calibration completes or times out.
      Co2CalibrationResult result = _manager.calibrate_co2();

      Event event {};
      event.type = EventType::Co2CalibrationDone;
      event.co2_cal_result = static_cast<uint8_t>(result);
      RTOS::queue_send(_event_queue, &event, 0);
      continue;
    }

    if (notify_value == NOTIFY_PREPARE) {
      // Run warmup after the orchestrator has powered the PM sensor on.
      // The first warmup read triggers the SPS30 read()
      // recovery path (stop → start → settle) which restarts measurement
      // automatically.  Blocks ~CONFIG_SENSOR_WARMUP_DURATION_MS.
      // The measurement notification latches during warmup and is consumed
      // on the next iteration of this loop.
      AG_LOGI(TAG, "PM prepare: warming up after power-on");
      _manager.warmup();
      AG_LOGI(TAG, "PM prepare complete");
      continue;
    }

    // Decode iteration count (bits 0-7) and sensor group mask (bits 8-15)
    uint32_t iterations = notify_value & 0xFF;
    SensorGroup groups = static_cast<SensorGroup>((notify_value >> 8) & 0xFF);

    // Guard against accidental zero values
    if (iterations == 0) {
      iterations = 1;
    }
    if (groups == SensorGroup::None) {
      groups = SensorGroup::All;
    }

    // Blocking call: with 1 iteration returns as fast as I2C reads complete.
    // The orchestrator continues processing other events while we block here.
    const Measures measures = _manager.start_measures(static_cast<int>(iterations), groups);

    // Map to MeasuresAGo — select the primary sensor channels only.
    MeasuresAGo basic {};
    basic.temp_hum_a = measures.temp_hum_a;
    basic.pm_a = measures.pm_a;
    basic.co2 = measures.co2;
    basic.tvoc_nox = measures.tvoc_nox;
    basic.power = measures.power;
    basic.pressure = measures.pressure;

    // TODO: Temporarily use raw value for index since algorithm not applied yet
    basic.tvoc_nox.tvoc_index = basic.tvoc_nox.tvoc_raw;
    basic.tvoc_nox.nox_index = basic.tvoc_nox.nox_raw;

    // Post result to the orchestrator event queue (non-blocking: drop if full).
    Event event {};
    event.type = EventType::SensorDataReady;
    event.sensor_data = basic;
    RTOS::queue_send(_event_queue, &event, 0);
  }
}
