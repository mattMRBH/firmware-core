/**
 * AirGradient Go — GPS Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_gps.h"

#include "go_events.h"
#include "rtos.h"

#ifndef TEST_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctime>
#include <sys/time.h>
#endif

static constexpr const char *TAG = "GpsService";

// Yield interval between GpsSensor::read() calls when the serial buffer is
// empty.  Keeps CPU usage low without introducing latency gaps larger than
// one NMEA epoch (~1 second).
static constexpr uint32_t TASK_YIELD_MS = 10;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GpsService::GpsService(GpsSensor &gps, QueueHandle_t event_queue, const Config &config)
    : _gps(gps), _event_queue(event_queue), _config(config) {
#ifndef TEST_HOST
  _mutex = xSemaphoreCreateMutex();
#endif
}

GpsService::~GpsService() {
  if (_running) {
    stop();
  }
#ifndef TEST_HOST
  if (_mutex != nullptr) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
#endif
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GpsService::start() {
#ifndef TEST_HOST
  if (_mutex == nullptr) {
    return false;
  }
  _clock_synced = false;
  _running = true;
  TaskHandle_t handle = nullptr;
  const BaseType_t ret = xTaskCreate(task_entry, "gps_task",
                                     static_cast<configSTACK_DEPTH_TYPE>(_config.task_stack_size),
                                     this, _config.task_priority, &handle);
  if (ret != pdPASS) {
    _running = false;
    return false;
  }
  _task_handle = handle;
  return true;
#else
  return false;
#endif
}

void GpsService::stop() {
  _running = false;
#ifndef TEST_HOST
  // Block until the task signals _done_sem just before self-deleting.
  if (_task_handle != nullptr && _done_sem != nullptr) {
    xSemaphoreTake(_done_sem, portMAX_DELAY);
    vSemaphoreDelete(_done_sem);
    _done_sem = nullptr;
    _task_handle = nullptr;
  }
#endif
}

GpsData GpsService::get_latest_fix() const {
#ifndef TEST_HOST
  xSemaphoreTake(_mutex, portMAX_DELAY);
  const GpsData snapshot = _latest_fix;
  xSemaphoreGive(_mutex);
  return snapshot;
#else
  return _latest_fix;
#endif
}

void GpsService::set_posting_interval_ms(int interval_ms) {
  _config.posting_interval_ms = interval_ms;
}

// ---------------------------------------------------------------------------
// Task entry point (static)
// ---------------------------------------------------------------------------

// static
void GpsService::task_entry(void *arg) {
  static_cast<GpsService *>(arg)->run();
#ifndef TEST_HOST
  vTaskDelete(nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Task loop
// ---------------------------------------------------------------------------

void GpsService::run() {
#ifndef TEST_HOST
  // Create the done semaphore inside the task so it is valid for the entire
  // task lifetime.  stop() blocks on this semaphore before returning.
  _done_sem = xSemaphoreCreateBinary();
#endif

  _gps.begin(_config.baud_rate);
  uint64_t last_post_ms = 0;

  while (_running) {
    if (_gps.read()) {
      const GpsData data = _gps.get_data();
      update_latest_fix(data);

      if (!_clock_synced && is_gps_timestamp_valid(data.timestamp)) {
        sync_system_clock(data.timestamp);
        _clock_synced = true;
      }
    }

    const uint64_t now_ms = RTOS::get_time_ms();
    if (now_ms - last_post_ms >= static_cast<uint64_t>(_config.posting_interval_ms)) {
      if (_gps.has_valid_fix()) {
        post_fix_event();
      }
      last_post_ms = now_ms;
    }

    RTOS::delay_ms(TASK_YIELD_MS);
  }

  _gps.end();

#ifndef TEST_HOST
  // Signal stop() that the task loop has exited before self-deleting.
  if (_done_sem != nullptr) {
    xSemaphoreGive(_done_sem);
  }
#endif
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void GpsService::update_latest_fix(const GpsData &data) {
#ifndef TEST_HOST
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _latest_fix = data;
  xSemaphoreGive(_mutex);
#else
  _latest_fix = data;
#endif
}

void GpsService::post_fix_event() {
  Event evt{};
  evt.type = EventType::GpsFixUpdate;
  evt.gps_data = _gps.get_data();
#ifndef TEST_HOST
  // Non-blocking send: drop the event if the queue is full.
  xQueueSend(_event_queue, &evt, 0);
#endif
}

void GpsService::sync_system_clock(const GpsTimestamp &ts) {
#ifndef TEST_HOST
  // GPS timestamps are UTC.  On ESP-IDF the default timezone is UTC, so
  // mktime() produces the correct POSIX epoch without timezone adjustment.
  struct tm t{};
  t.tm_year = ts.year - 1900;
  t.tm_mon = ts.month - 1;
  t.tm_mday = ts.day;
  t.tm_hour = ts.hour;
  t.tm_min = ts.minute;
  t.tm_sec = ts.second;
  t.tm_isdst = 0;
  const time_t epoch = mktime(&t);
  if (epoch != static_cast<time_t>(-1)) {
    const struct timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
  }
#else
  (void)ts;
#endif
}

// ---------------------------------------------------------------------------
// One-shot synchronous read (fast-path timer wake)
// ---------------------------------------------------------------------------

GpsData gps_read_once(GpsSensor &gps, int baud_rate, uint32_t timeout_ms) {
  gps.begin(baud_rate);
  const uint64_t deadline_ms = RTOS::get_time_ms() + timeout_ms;
  while (RTOS::get_time_ms() < deadline_ms) {
    if (gps.read() && gps.has_valid_fix()) {
      break;
    }
    RTOS::delay_ms(10);
  }
  const GpsData data = gps.get_data();
  gps.end();
  return data;
}
