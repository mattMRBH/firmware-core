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
 * @brief RTOS abstraction interface for platform-independent time and delay
 * operations
 *
 * This interface provides abstraction for FreeRTOS/ESP-IDF timing functions to
 * enable host-side testing without hardware dependencies.
 *
 * Uses static facade pattern: static methods delegate to mockable virtual
 * implementations.
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
   * @brief Create and start a FreeRTOS task.
   *
   * Blocking: no
   * Allocates: yes (FreeRTOS task stack)
   * No-op in TEST_HOST mode (returns false).
   *
   * @param func        Task entry function.
   * @param name        Task name string (debug only).
   * @param stack_depth Stack depth in words.
   * @param param       Argument passed to func.
   * @param priority    FreeRTOS task priority.
   * @param out_handle  Receives the opaque task handle; may be nullptr.
   * @return true on success; false on failure or in TEST_HOST mode.
   */
  static bool task_create(void (*func)(void *), const char *name, uint32_t stack_depth, void *param,
                          uint32_t priority, void **out_handle);

  /**
   * @brief Delete a FreeRTOS task.
   *
   * No-op in TEST_HOST mode.
   *
   * @param handle Opaque task handle from task_create(); nullptr deletes the
   * calling task.
   */
  static void task_delete(void *handle);

  /**
   * @brief Create a FreeRTOS queue.
   *
   * Allocates: yes (FreeRTOS queue storage)
   * Returns nullptr in TEST_HOST mode.
   *
   * @param length    Maximum number of items the queue can hold.
   * @param item_size Size in bytes of each item.
   * @return Opaque queue handle, or nullptr on failure / in TEST_HOST mode.
   */
  static void *queue_create(uint32_t length, uint32_t item_size);

  /**
   * @brief Delete a FreeRTOS queue created with queue_create().
   *
   * No-op in TEST_HOST mode or when queue_handle is nullptr.
   *
   * @param queue_handle Opaque queue handle from queue_create().
   */
  static void queue_delete(void *queue_handle);

  /**
   * @brief Send an item to a FreeRTOS queue.
   *
   * Thread-safe: yes (FreeRTOS queue)
   * No-op in TEST_HOST mode.
   *
   * @param queue_handle Opaque queue handle (QueueHandle_t on hardware).
   * @param item         Pointer to the item to copy into the queue.
   * @param timeout_ms   Ticks to wait if the queue is full; 0 = drop
   * immediately.
   */
  static void queue_send(void *queue_handle, const void *item, uint32_t timeout_ms = 0);

  /**
   * @brief Receive an item from a FreeRTOS queue.
   *
   * Thread-safe: yes (FreeRTOS queue)
   * Always returns false in TEST_HOST mode.
   *
   * @param queue_handle Opaque queue handle (QueueHandle_t on hardware).
   * @param item         Buffer to receive the item into.
   * @param timeout_ms   Maximum time to wait; 0 = non-blocking,
   *                     UINT32_MAX = wait indefinitely (portMAX_DELAY).
   * @return true if an item was received; false on timeout or TEST_HOST.
   */
  static bool queue_receive(void *queue_handle, void *item, uint32_t timeout_ms);

  /**
   * @brief Send an item to a FreeRTOS queue from ISR context.
   *
   * ISR-safe: yes
   * No-op in TEST_HOST mode.
   * Calls portYIELD_FROM_ISR() internally when a higher-priority task is
   * woken.
   *
   * @param queue_handle Opaque queue handle (QueueHandle_t on hardware).
   * @param item         Pointer to the item to copy into the queue.
   */
  static void queue_send_from_isr(void *queue_handle, const void *item);

  /**
   * @brief Apply a UTC epoch timestamp to the platform system clock.
   *
   * No-op in TEST_HOST mode.
   *
   * @param epoch_seconds POSIX epoch (seconds since 1970-01-01T00:00:00Z).
   */
  static void set_system_time_from_epoch(int64_t epoch_seconds);

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

/**
 * @brief RAII mutex wrapper.
 *
 * Wraps a FreeRTOS mutex semaphore.  The mutex is created in the constructor
 * and deleted in the destructor.  In TEST_HOST mode all operations are no-ops
 * and is_valid() returns false.
 *
 * Thread-safe: yes (FreeRTOS mutex semantics on hardware)
 * Blocking: lock() waits indefinitely until the mutex is available.
 * Not copyable or movable.
 */
class RtosMutex {
public:
  /// Create the underlying mutex.  is_valid() returns false if creation fails
  /// or when running under TEST_HOST.
  RtosMutex();
  ~RtosMutex();

  RtosMutex(const RtosMutex &) = delete;
  RtosMutex &operator=(const RtosMutex &) = delete;

  /// Acquire the mutex, waiting indefinitely.
  /// @return true on success; always true in TEST_HOST mode.
  bool lock();

  /// Release the mutex.
  void unlock();

  /// @return true if the underlying handle was created successfully.
  bool is_valid() const;

private:
  void *_handle; ///< SemaphoreHandle_t on hardware; nullptr in TEST_HOST.
};

/**
 * @brief RAII binary semaphore wrapper with deferred creation.
 *
 * Unlike RtosMutex, the semaphore handle is not created in the constructor.
 * Call create() before any take()/give() calls.  This matches the FreeRTOS
 * pattern where a completion semaphore is created inside the task that will
 * later signal it.
 *
 * In TEST_HOST mode all operations are no-ops.
 * Not copyable or movable.
 */
class RtosBinarySemaphore {
public:
  RtosBinarySemaphore() = default;
  ~RtosBinarySemaphore();

  RtosBinarySemaphore(const RtosBinarySemaphore &) = delete;
  RtosBinarySemaphore &operator=(const RtosBinarySemaphore &) = delete;

  /// Create the underlying binary semaphore.
  /// @return true on success; always true in TEST_HOST mode.
  bool create();

  /// Wait indefinitely for the semaphore to be given.
  /// @return true on success; always true in TEST_HOST mode.
  bool take();

  /// Signal (give) the semaphore.
  /// @return true on success; always true in TEST_HOST mode.
  bool give();

  /// Delete the underlying semaphore and reset to the uncreated state.
  void destroy();

  /// @return true if create() has been called and the handle is live.
  bool is_created() const;

private:
  /// SemaphoreHandle_t on hardware; non-null sentinel after create() in
  /// TEST_HOST.
  void *_handle = nullptr;
};

#endif // RTOS_H
