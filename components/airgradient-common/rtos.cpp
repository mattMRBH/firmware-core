/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "rtos.h"

#ifndef TEST_HOST
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctime>
#include <sys/time.h>
#endif

// ---------------------------------------------------------------------------
// RTOS singleton
// ---------------------------------------------------------------------------

// Singleton instance storage
static RTOS *s_rtos_instance = nullptr;

RTOS *RTOS::get_instance() {
#ifndef TEST_HOST
  if (s_rtos_instance == nullptr) {
    // Auto-create default implementation if not set (production only)
    static FreeRTOS default_impl;
    s_rtos_instance = &default_impl;
  }
#endif
  return s_rtos_instance;
}

void RTOS::set_instance(RTOS *rtos) { s_rtos_instance = rtos; }

// ---------------------------------------------------------------------------
// Timing facade (mockable via virtual singleton)
// ---------------------------------------------------------------------------

// Static facade methods - delegate to singleton instance
void RTOS::delay_ms(uint32_t ms) { get_instance()->delay_ms_impl(ms); }

uint64_t RTOS::get_time_ms() { return get_instance()->get_time_ms_impl(); }

// FreeRTOS implementation
void FreeRTOS::delay_ms_impl(uint32_t ms) {
#ifndef TEST_HOST
  vTaskDelay(pdMS_TO_TICKS(ms));
#endif
}

uint64_t FreeRTOS::get_time_ms_impl() {
#ifndef TEST_HOST
  return esp_timer_get_time() / 1000;
#else
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// Task lifecycle (no-op stubs in TEST_HOST)
// ---------------------------------------------------------------------------

bool RTOS::task_create(void (*func)(void *), const char *name, uint32_t stack_depth, void *param,
                       uint32_t priority, void **out_handle) {
#ifndef TEST_HOST
  TaskHandle_t handle = nullptr;
  const BaseType_t ret = xTaskCreate(func, name, static_cast<configSTACK_DEPTH_TYPE>(stack_depth),
                                     param, static_cast<UBaseType_t>(priority), &handle);
  if (out_handle != nullptr) {
    *out_handle = handle;
  }
  return ret == pdPASS;
#else
  (void)func;
  (void)name;
  (void)stack_depth;
  (void)param;
  (void)priority;
  (void)out_handle;
  return false;
#endif
}

void RTOS::task_delete(void *handle) {
#ifndef TEST_HOST
  vTaskDelete(static_cast<TaskHandle_t>(handle));
#else
  (void)handle;
#endif
}

// ---------------------------------------------------------------------------
// Queue (no-op stubs in TEST_HOST)
// ---------------------------------------------------------------------------

void *RTOS::queue_create(uint32_t length, uint32_t item_size) {
#ifndef TEST_HOST
  return xQueueCreate(static_cast<UBaseType_t>(length), static_cast<UBaseType_t>(item_size));
#else
  (void)length;
  (void)item_size;
  return nullptr;
#endif
}

void RTOS::queue_delete(void *queue_handle) {
#ifndef TEST_HOST
  if (queue_handle != nullptr) {
    vQueueDelete(static_cast<QueueHandle_t>(queue_handle));
  }
#else
  (void)queue_handle;
#endif
}

void RTOS::queue_send(void *queue_handle, const void *item, uint32_t timeout_ms) {
#ifndef TEST_HOST
  xQueueSend(static_cast<QueueHandle_t>(queue_handle), item, pdMS_TO_TICKS(timeout_ms));
#else
  (void)queue_handle;
  (void)item;
  (void)timeout_ms;
#endif
}

bool RTOS::queue_receive(void *queue_handle, void *item, uint32_t timeout_ms) {
#ifndef TEST_HOST
  const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xQueueReceive(static_cast<QueueHandle_t>(queue_handle), item, ticks) == pdTRUE;
#else
  (void)queue_handle;
  (void)item;
  (void)timeout_ms;
  return false;
#endif
}

void RTOS::queue_send_from_isr(void *queue_handle, const void *item) {
#ifndef TEST_HOST
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(static_cast<QueueHandle_t>(queue_handle), item, &woken);
  portYIELD_FROM_ISR(woken);
#else
  (void)queue_handle;
  (void)item;
#endif
}

// ---------------------------------------------------------------------------
// System clock (no-op stub in TEST_HOST)
// ---------------------------------------------------------------------------

void RTOS::set_system_time_from_epoch(int64_t epoch_seconds) {
#ifndef TEST_HOST
  const struct timeval tv = {static_cast<time_t>(epoch_seconds), 0};
  settimeofday(&tv, nullptr);
#else
  (void)epoch_seconds;
#endif
}

// ---------------------------------------------------------------------------
// RtosMutex
// ---------------------------------------------------------------------------

RtosMutex::RtosMutex() : _handle(nullptr) {
#ifndef TEST_HOST
  _handle = xSemaphoreCreateMutex();
#endif
}

RtosMutex::~RtosMutex() {
#ifndef TEST_HOST
  if (_handle != nullptr) {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(_handle));
    _handle = nullptr;
  }
#endif
}

bool RtosMutex::lock() {
#ifndef TEST_HOST
  return xSemaphoreTake(static_cast<SemaphoreHandle_t>(_handle), portMAX_DELAY) == pdTRUE;
#else
  return true;
#endif
}

void RtosMutex::unlock() {
#ifndef TEST_HOST
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(_handle));
#endif
}

bool RtosMutex::is_valid() const { return _handle != nullptr; }

// ---------------------------------------------------------------------------
// RtosBinarySemaphore
// ---------------------------------------------------------------------------

RtosBinarySemaphore::~RtosBinarySemaphore() { destroy(); }

bool RtosBinarySemaphore::create() {
#ifndef TEST_HOST
  _handle = xSemaphoreCreateBinary();
  return _handle != nullptr;
#else
  // Use a non-null sentinel so is_created() returns true on TEST_HOST.
  // The value is never dereferenced outside of #ifndef TEST_HOST guards.
  _handle = reinterpret_cast<void *>(1);
  return true;
#endif
}

bool RtosBinarySemaphore::take() {
#ifndef TEST_HOST
  return xSemaphoreTake(static_cast<SemaphoreHandle_t>(_handle), portMAX_DELAY) == pdTRUE;
#else
  return true;
#endif
}

bool RtosBinarySemaphore::give() {
#ifndef TEST_HOST
  return xSemaphoreGive(static_cast<SemaphoreHandle_t>(_handle)) == pdTRUE;
#else
  return true;
#endif
}

void RtosBinarySemaphore::destroy() {
#ifndef TEST_HOST
  if (_handle != nullptr) {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(_handle));
  }
#endif
  _handle = nullptr;
}

bool RtosBinarySemaphore::is_created() const { return _handle != nullptr; }
