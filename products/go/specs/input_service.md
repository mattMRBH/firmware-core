# Input Service — Implementation Spec

Product-specific input service for AirGradient Go. Runs as an independent
FreeRTOS task that handles all user input: 3 capacitive touch pads (via CAP1203)
and 2 physical buttons (via GPIO). Classifies raw hardware events into typed
`InputPress` events and posts them to the orchestrator event queue.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_input.h` | `InputService` class declaration |
| `products/go/main/go_input.cpp` | ISR handlers, task loop, debounce/classification logic |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `CapTouchSensor` (CAP1203) | `airgradient-touch` | Read touch pad state, clear interrupt |
| `gpio::Hal` | `airgradient-gpio` | Physical button GPIO interrupts |
| `go_types.h` | product | `InputSource`, `InputType` enums |
| `go_events.h` | product | `Event`, `EventType::InputPress` |
| FreeRTOS queue | ESP-IDF / RTOS | Internal raw queue + orchestrator event queue |

## Hardware Inputs

| Input | Hardware | GPIO/Channel | Interrupt Source |
|---|---|---|---|
| Touch Up | CAP1203 CH1 | I2C (+ INT pin on GPIO) | CAP1203 INT line |
| Touch Down | CAP1203 CH2 | I2C (+ INT pin on GPIO) | CAP1203 INT line |
| Touch Enter | CAP1203 CH3 | I2C (+ INT pin on GPIO) | CAP1203 INT line |
| Button Power | Physical GPIO | board_config.h | GPIO falling edge |
| Button Boot | Physical GPIO | board_config.h | GPIO falling edge |

All pin assignments come from `board_config.h`. The CAP1203 INT pin is a GPIO
that fires when any enabled touch channel detects a touch event.

## Class Design

```cpp
#pragma once

#include "airgradient_gpio.h"
#include "cap_touch_sensor.h"
#include "go_events.h"
#include "go_types.h"

#include <cstdint>

struct QueueDefinition;
typedef QueueDefinition *QueueHandle_t;

class InputService {
  public:
    struct Config {
        int pin_cap_int;                     // CAP1203 INT pin (GPIO)
        int pin_button_power;                // physical button 1
        int pin_button_boot;                 // physical button 2
        uint32_t debounce_ms          = 50;  // debounce window for physical buttons
        uint32_t long_press_ms        = 2000; // long press threshold (physical only)
        uint16_t task_stack_size      = 3072;
        uint8_t task_priority         = 6;
    };

    InputService(CapTouchSensor &touch, const gpio::Hal &gpio,
                 QueueHandle_t event_queue, const Config &config);

    /// Start the input processing task and register ISR handlers.
    bool start();

    /// Stop the task and unregister ISR handlers.
    void stop();

  private:
    CapTouchSensor &_touch;
    const gpio::Hal &_gpio;
    QueueHandle_t _event_queue;
    Config _config;

    QueueHandle_t _raw_queue;           // internal ISR -> task queue
    volatile bool _running = false;
    TaskHandle_t _task_handle = nullptr;

    // ISR handlers (static, minimal: just post to _raw_queue)
    static void cap_int_isr(void *arg);
    static void button_power_isr(void *arg);
    static void button_boot_isr(void *arg);

    static void task_entry(void *arg);
    void run();

    void process_touch_interrupt();
    void process_button_event(InputSource source, uint64_t timestamp_ms);
    void post_input_event(InputSource source, InputType type);
};
```

## Internal Raw Event

The ISR handlers post a minimal struct to the internal raw queue. This struct
must be ISR-safe (no heap, no strings, trivially copyable):

```cpp
struct RawInputEvent {
    InputSource source;  // which input triggered
    uint64_t timestamp;  // RTOS time at ISR (or 0 if unavailable from ISR)
};
```

For touch events, `source` is set to a sentinel value (e.g. `TouchEnter` as a
placeholder) since the actual channel is determined by reading CAP1203 in
task context. The ISR only signals "a touch happened on some channel."

For physical buttons, `source` is `ButtonPower` or `ButtonBoot`.

## Task Loop

```
InputService::run():
    register ISR for CAP1203 INT pin (falling edge)
    register ISR for button_power pin (falling edge)
    register ISR for button_boot pin (falling edge)

    while (_running):
        // Block on raw queue with timeout (for long-press timing)
        if xQueueReceive(_raw_queue, &raw, timeout):
            switch raw.source:
                case touch sentinel:
                    process_touch_interrupt()

                case ButtonPower:
                case ButtonBoot:
                    process_button_event(raw.source, raw.timestamp)

        // Check for pending long-press timeouts
        check_pending_long_press()
```

### Touch Processing

When the CAP1203 INT fires:

```
process_touch_interrupt():
    TouchData data
    if !_touch.read(data):
        return  // I2C failure, skip
    _touch.clear_interrupt()  // CAP1203-specific, clears INT line

    // Map channel bitmask to InputSource
    if data.touched & TouchChannel::CH1:
        post_input_event(InputSource::TouchUp, InputType::ShortPress)
    if data.touched & TouchChannel::CH2:
        post_input_event(InputSource::TouchDown, InputType::ShortPress)
    if data.touched & TouchChannel::CH3:
        post_input_event(InputSource::TouchEnter, InputType::ShortPress)
```

Touch pads only produce `ShortPress`. No long-press detection for touch.

Note: The channel-to-InputSource mapping (CH1=Up, CH2=Down, CH3=Enter) is
defined in the implementation. If the PCB layout maps channels differently,
this mapping is the single place to change.

### Physical Button Processing

Physical buttons require debounce and long-press detection:

```
process_button_event(source, timestamp):
    // Debounce: ignore if too close to last event for this source
    if (timestamp - last_event_time[source]) < debounce_ms:
        return

    last_event_time[source] = timestamp

    // Record press-down time, start long-press timer
    press_start_time[source] = timestamp
    pending_long_press[source] = true

check_pending_long_press():
    now = RTOS::get_time_ms()
    for each source with pending_long_press:
        if (now - press_start_time[source]) >= long_press_ms:
            // Check if button is still held (read GPIO level)
            if gpio.get_level(pin_for(source)) == PRESSED_LEVEL:
                post_input_event(source, InputType::LongPress)
                pending_long_press[source] = false
            else:
                // Button was released before long-press threshold
                post_input_event(source, InputType::ShortPress)
                pending_long_press[source] = false
```

Alternative approach: use a second ISR on the rising edge (button release) to
measure press duration directly. The polling approach above is simpler and
avoids double-interrupt configuration.

**Design choice**: The task uses `xQueueReceive` with a timeout equal to the
remaining time until the nearest pending long-press expires. This avoids
busy-polling while still detecting long presses accurately.

### Event Posting

```cpp
void InputService::post_input_event(InputSource source, InputType type) {
    Event event;
    event.type = EventType::InputPress;
    event.input.source = source;
    event.input.type = type;
    xQueueSend(_event_queue, &event, 0);  // non-blocking, drop if full
}
```

## Deep Sleep Wake Sources

Physical button GPIOs serve as deep sleep wake sources. The Power Management
service configures these pins as wake sources before entering deep sleep. The
Input Service does not manage sleep wake configuration — it only runs when the
device is awake.

On wake from button press, the orchestrator starts the Input Service. The first
button press (the one that woke the device) is not processed as an input event;
the `WakeFromSleep(Button)` event handles that case.

## Configuration

| Config Field | Source | Notes |
|---|---|---|
| `pin_cap_int` | `board_config.h` | CAP1203 ALERT/INT output pin |
| `pin_button_power` | `board_config.h` | Power/lock/unlock physical button |
| `pin_button_boot` | `board_config.h` | Boot/factory-reset physical button |
| `debounce_ms` | hardcoded (50ms) | Typical for mechanical buttons |
| `long_press_ms` | hardcoded (2000ms) | 2 seconds for long press |
| `task_stack_size` | hardcoded | Tuned at integration time |
| `task_priority` | hardcoded | Above GPS, at or above sensor task |

## CAP1203 Noise Handling

The CAP1203 supports noise detection. When `TouchData.noise` flags are set for
a channel, that channel's touch reading is unreliable. The Input Service should
ignore touched channels that also have their noise flag set:

```cpp
uint8_t valid_touches = data.touched & ~data.noise;
```

If noise is persistent, the service can attempt a recalibration:
```cpp
if (data.noise != 0 && _touch.supports_calibration()) {
    _touch.calibrate(data.noise);
}
```

## Testability

For host testing under `TEST_HOST`:

- Inject a mock `CapTouchSensor` that returns controlled `TouchData`
- Construct a `gpio::Hal` with mock function pointers for GPIO reads and
  interrupt registration
- Replace FreeRTOS queues with test doubles
- Test classification logic: feed raw events, assert correct `InputEventData`
  output
- Test debounce: feed events with close timestamps, assert deduplication
- Test long-press: simulate held button (GPIO level stays low), assert
  `LongPress` after threshold

The debounce and classification logic can be extracted into pure functions for
unit testing independently of the task infrastructure.
