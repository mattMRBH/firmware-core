# Sensor Producer — Implementation Spec

Product-specific sensor producer for AirGradient Go. Wraps the shared
`SensorManager` in an independent FreeRTOS task. The orchestrator signals it
to run a measurement cycle; the task calls `SensorManager::start_measures()`
(which blocks for the full averaging duration) and posts a `SensorDataReady`
event to the orchestrator event queue when done.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_sensor_producer.h` | `SensorProducer` class declaration |
| `products/go/main/go_sensor_producer.cpp` | Task loop, notification handling, event posting |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `SensorManager` | `airgradient-sensors` (service) | Blocking multi-iteration sensor averaging |
| `Sensors` struct | `airgradient-sensors` (service) | HAL pointer table injected into SensorManager |
| `go_events.h` | product | `Event`, `EventType::SensorDataReady` |
| `measures_types.h` | `airgradient-common` | `Measures` struct |
| FreeRTOS task notifications | ESP-IDF / RTOS | Orchestrator → task signalling |
| FreeRTOS queue | ESP-IDF / RTOS | `xQueueSend` to orchestrator event queue |

## AGo Sensor Wiring

AGo wires the following sensors into the `Sensors` struct. All unused pointers
are explicitly set to `nullptr` — SensorManager skips `nullptr` sensors safely.

| Sensors field | Driver | Notes |
|---|---|---|
| `temp_hum` | `SHT40` | Dedicated temperature + humidity sensor |
| `co2` | `S8` or `Sunlight` | CO2 sensor (exact model TBD at board bring-up) |
| `pms_a` | `PMS5003` | Single particulate matter sensor |
| `pms_b` | `nullptr` | No second PM sensor on AGo |
| `tvoc_nox` | `SGP41` | TVOC + NOx sensor |
| `o3_no2` | `nullptr` | No AlphaSense electrodes on AGo |
Battery management is handled separately by `PowerService` via the
`airgradient-bms` component and is not part of the `Sensors` struct.

## Class Design

```cpp
#pragma once

#include "go_events.h"
#include "measures_types.h"
#include "services/sensor_manager.h"

#include <cstdint>

struct QueueDefinition;
typedef QueueDefinition *QueueHandle_t;
struct tskTaskControlBlock;
typedef tskTaskControlBlock *TaskHandle_t;

class SensorProducer {
  public:
    struct Config {
        uint16_t task_stack_size = 4096;
        uint8_t task_priority    = 5;
    };

    SensorProducer(SensorManager &manager, QueueHandle_t event_queue,
                   const Config &config);

    /// Start the sensor task. Call once during initialization.
    /// Returns true if the task was created successfully.
    bool start();

    /// Stop the sensor task. Blocks until the task exits.
    void stop();

    /// Trigger one measurement cycle with the given iteration count.
    /// The orchestrator calls this to request a measurement.
    /// Non-blocking: returns immediately after signalling the task.
    void request_measurement(uint8_t iterations);

  private:
    SensorManager &_manager;
    QueueHandle_t _event_queue;
    Config _config;

    volatile bool _running   = false;
    TaskHandle_t _task_handle = nullptr;

    static void task_entry(void *arg);
    void run();
};
```

## Task Loop

```
SensorProducer::run():
    while (_running):
        // Block indefinitely until the orchestrator signals a measurement
        uint32_t iterations = 0
        xTaskNotifyWait(0, 0, &iterations, portMAX_DELAY)

        if !_running:
            break   // stop() was called

        if iterations == 0:
            iterations = 1  // guard against accidental zero

        // Blocking call — takes (iterations * CONFIG_AVERAGING_ITERATION_INTERVAL_MS)
        Measures measures = _manager.start_measures(static_cast<int>(iterations))

        // Post result to orchestrator event queue
        Event event
        event.type = EventType::SensorDataReady
        event.sensor_data = measures
        xQueueSend(_event_queue, &event, 0)  // non-blocking, drop if full
```

## Orchestrator Signalling

The orchestrator calls `request_measurement(iterations)` when a
`MeasurementTimer` event fires. Internally this uses `xTaskNotify`:

```cpp
void SensorProducer::request_measurement(uint8_t iterations) {
    if (_task_handle != nullptr) {
        xTaskNotify(_task_handle,
                    static_cast<uint32_t>(iterations),
                    eSetValueWithOverwrite);
    }
}
```

`eSetValueWithOverwrite` ensures the iteration count is always updated even
if the previous notification has not been consumed yet. This prevents stale
iteration counts if the orchestrator fires two timers in quick succession
(which should not happen in normal operation, but is safe by design).

## Iteration Count

The orchestrator passes the iteration count per cycle. This allows the
orchestrator to adapt based on context:

| Scenario | Iterations |
|---|---|
| Normal measurement cycle | Derived from `measurement_interval_seconds` and `CONFIG_AVERAGING_ITERATION_INTERVAL_MS` (e.g. 60s / 2s = 30 iterations) |
| First measurement on boot | 1 iteration (get a reading quickly) |
| Low battery mode (future) | 1 iteration (reduce active time) |

The orchestrator computes the iteration count as:

```
iterations = max(1, measurement_interval_seconds * 1000
                    / CONFIG_AVERAGING_ITERATION_INTERVAL_MS)
```

`CONFIG_AVERAGING_ITERATION_INTERVAL_MS` defaults to 2000ms (defined in
`sensor_manager.cpp`).

## Task Duration

The sensor task is blocked for the full measurement duration:
`iterations × CONFIG_AVERAGING_ITERATION_INTERVAL_MS`. During this time the
orchestrator continues processing other events (GPS fixes, button presses,
timers) uninterrupted. This is the primary reason for the independent task —
SensorManager is inherently blocking.

## stop() Behaviour

`stop()` must unblock the waiting task so it can exit cleanly:

```cpp
void SensorProducer::stop() {
    _running = false;
    if (_task_handle != nullptr) {
        // Unblock the task if it is waiting for a notification
        xTaskNotify(_task_handle, 0, eSetValueWithOverwrite);
        // Wait for the task to exit
        // Use a semaphore or vTaskDelete depending on implementation preference
    }
}
```

Simplest approach: call `vTaskDelete(_task_handle)` from the outside and set
`_task_handle = nullptr`. This is safe if the sensor task is not holding any
mutex at deletion time. SensorManager does not use mutexes internally, so
this is acceptable.

## Sensors Struct Wiring

The `Sensors` struct and concrete driver instances are owned by the product
wiring layer (in `main.cpp` or a BSP initialisation function), not by
`SensorProducer`. The wiring layer:

1. Initialises I2C/UART buses
2. Constructs concrete driver instances (`SHT40`, `S8`/`Sunlight`, `PMS5003`,
   `SGP41`)
3. Calls `driver.init()` on each
4. Populates a `Sensors` struct (unused pointers set to `nullptr`)
5. Constructs `SensorManager(sensors)`
6. Constructs `SensorProducer(sensor_manager, event_queue, config)`
7. Calls `sensor_producer.start()`

`SensorProducer` only holds a reference to `SensorManager`. It has no
knowledge of which sensors are wired.

## Relationship with PowerService (BMS)

`SensorManager` handles only environmental sensors. Battery management is
entirely separate -- the orchestrator reads battery status independently from
`PowerService::poll_bms()` (which uses the `airgradient-bms` component) on a
periodic timer, separate from the measurement cycle. There is no I2C
contention because both accesses happen in the orchestrator context (the BMS
poll timer fires outside the sensor measurement window).

## Testability

For host testing under `TEST_HOST`:

- Inject a mock `SensorManager` (or construct a `SensorManager` with mock
  sensor HALs via the existing `Sensors` struct pattern)
- Replace the FreeRTOS queue and task notification with test doubles
- Test `request_measurement(N)` → verify the task calls
  `start_measures(N)` → verify `SensorDataReady` event posted with correct
  `Measures` payload
- Test zero-iteration guard: `request_measurement(0)` → task uses 1 iteration

The measurement logic itself lives entirely in `SensorManager` (which has its
own host tests in `tests/`). `SensorProducer` is thin task infrastructure —
the main things to verify are the notification handoff and correct event
construction.
