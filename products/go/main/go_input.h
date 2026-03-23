/**
 * AirGradient Go — Input Service
 *
 * Runs as an independent RTOS task that handles all user input: 3
 * capacitive touch pads (via CAP1203) and 2 physical buttons (via GPIO).
 * Classifies raw hardware events into typed InputPress events and posts them
 * to the orchestrator event queue.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "airgradient_gpio.h"
#include "cap_touch_sensor.h"
#include "go_events.h"
#include "go_types.h"
#include "rtos.h"

#include <cstdint>

class InputService {
public:
  struct Config {
    int pin_cap_int;                 // CAP1203 INT pin (GPIO)
    int pin_button_power;            // physical button 1 (Power/lock)
    int pin_button_boot;             // physical button 2 (Boot/factory-reset)
    uint32_t debounce_ms = 50;       // debounce window for physical buttons
    uint32_t long_press_ms = 2000;   // long-press threshold (physical only)
    uint16_t task_stack_size = 3072; // RTOS task stack words
    uint8_t task_priority = 6;       // RTOS task priority
  };

  /// Construct the service.  Does not start the task.
  ///
  /// @param touch        CAP1203 touch sensor driver (or mock).  Must outlive
  ///                     this service.
  /// @param gpio         GPIO HAL function-pointer table.
  /// @param event_queue  Orchestrator event queue.  Must be created before
  ///                     start() is called.
  /// @param config       Runtime configuration (pins, debounce, …).
  InputService(CapTouchSensor &touch, const gpio::Hal &gpio, RtosQueueHandle event_queue,
               const Config &config);
  ~InputService();

  /// Start the input processing task and register ISR handlers.
  /// Returns true if the task was created successfully.
  bool start();

  /// Stop the task and unregister ISR handlers.
  void stop();

private:
  CapTouchSensor &_touch;
  const gpio::Hal &_gpio;
  RtosQueueHandle _event_queue;
  Config _config;

  RtosQueueHandle _raw_queue; // internal ISR -> task queue
  volatile bool _running = false;
  RtosTaskHandle _task_handle = nullptr;
  RtosBinarySemaphore _done_sem; // signalled by task before self-delete

  // Per-button state for debounce and long-press detection.
  // Index 0 = ButtonPower, index 1 = ButtonBoot.
  // _first_press sentinel: true means the button has never been pressed, so
  // the debounce window is skipped and the first press is always accepted.
  bool _first_press[2] = {true, true};
  uint64_t _last_event_time_ms[2] = {0, 0};
  uint64_t _press_start_time_ms[2] = {0, 0};
  bool _pending_long_press[2] = {false, false};

protected:
  /// Read CAP1203 status, clear interrupt, map channels to InputSource,
  /// and post ShortPress events for each valid (non-noisy) channel.
  void process_touch_interrupt();

  /// Debounce a physical button event and start a long-press timer.
  void process_button_event(InputSource source, uint64_t timestamp_ms);

  /// Check all pending long-press timers; fire LongPress or ShortPress
  /// once the threshold is reached.
  void check_pending_long_press();

  /// Compute the timeout (ms) to pass to RTOS::queue_receive so the task wakes
  /// up when the nearest pending long-press timer expires.
  /// Returns UINT32_MAX (portMAX_DELAY equivalent) when no presses are pending.
  uint32_t compute_queue_timeout_ms() const;

  /// Post a typed input event to the orchestrator queue (non-blocking).
  /// Virtual to allow test subclasses to capture events without a real queue.
  virtual void post_input_event(InputSource source, InputType type);

private:
  // ISR handlers (static, minimal: post raw event to _raw_queue).
  static void cap_int_isr(void *arg);
  static void button_power_isr(void *arg);
  static void button_boot_isr(void *arg);

  static void task_entry(void *arg);
  void run();

  /// Map a button index (0=Power, 1=Boot) to its GPIO pin number.
  int pin_for_button_index(int idx) const;
};
