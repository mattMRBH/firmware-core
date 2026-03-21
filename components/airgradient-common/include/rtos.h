/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef RTOS_H
#define RTOS_H

#include <stdint.h>

/**
 * @brief RTOS abstraction interface for platform-independent time and delay operations
 *
 * This interface provides abstraction for FreeRTOS/ESP-IDF timing functions to enable
 * host-side testing without hardware dependencies.
 *
 * Uses static facade pattern: static methods delegate to mockable virtual implementations.
 */
class RTOS {
public:
  virtual ~RTOS() = default;

  /**
   * @brief Delay execution for specified milliseconds (static facade)
   * @param ms Milliseconds to delay
   */
  static void delay_ms(uint32_t ms);

  /**
   * @brief Get current time in milliseconds since boot (static facade)
   * @return Time in milliseconds
   */
  static uint64_t get_time_ms();

  /**
   * @brief Set singleton instance (primarily for testing)
   * @param rtos Pointer to RTOS implementation
   */
  static void set_instance(RTOS *rtos);

  /**
   * @brief Virtual implementation of delay (mockable)
   * @param ms Milliseconds to delay
   */
  virtual void delay_ms_impl(uint32_t ms) = 0;

  /**
   * @brief Virtual implementation of get_time_ms (mockable)
   * @return Time in milliseconds
   */
  virtual uint64_t get_time_ms_impl() = 0;

private:
  static RTOS *get_instance();
};

/**
 * @brief FreeRTOS implementation of RTOS interface
 *
 * Uses vTaskDelay() and esp_timer_get_time() for ESP-IDF platform.
 */
class FreeRTOS : public RTOS {
public:
  void delay_ms_impl(uint32_t ms) override;
  uint64_t get_time_ms_impl() override;
};

#endif // RTOS_H
