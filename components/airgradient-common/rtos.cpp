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
#include "freertos/task.h"
#endif

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
