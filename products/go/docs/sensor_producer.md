# Sensor Producer

Independent FreeRTOS task that drives sensor measurements for AirGradient Go.
Wraps the shared `SensorManager` component: the orchestrator signals it with
an iteration count via FreeRTOS task notification; the task blocks inside
`SensorManager::start_measures()` for the full averaging window, then posts a
`SensorDataReady` event to the orchestrator queue.

`SensorProducer` holds only a `SensorManager` reference and has no knowledge
of which sensors are wired — that is the product wiring layer's responsibility
(`main.cpp`).

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_sensor_producer.h` | `SensorProducer` class declaration, `Config` struct |
| `products/go/main/go_sensor_producer.cpp` | Task loop, notification handling, event posting |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `SensorManager` | `airgradient-sensors` (`services/sensor_manager.h`) | Blocking multi-iteration sensor averaging |
| `Sensors` struct | `airgradient-sensors` (`services/sensor_manager.h`) | HAL pointer table injected into `SensorManager` by wiring layer |
| `Measures` | `airgradient-common` (`measures_types.h`) | Averaging result; carried in `SensorDataReady` event payload |
| `RTOS` | `airgradient-common` (`rtos.h`) | `task_create()`, `task_delete()`, `queue_send()` |
| `go_events.h` | product | `Event`, `EventType::SensorDataReady` |
| FreeRTOS task notifications | ESP-IDF / FreeRTOS | `xTaskNotify` / `xTaskNotifyWait` — orchestrator → task signalling |

## AGo Sensor Wiring

The wiring layer (`main.cpp`) populates a `Sensors` struct and passes it to
`SensorManager`. `SensorProducer` never touches this struct directly.

| `Sensors` field | Driver | Notes |
|---|---|---|
| `temp_hum` | `SHT40` | Dedicated temperature + humidity sensor |
| `co2` | `S8` or `Sunlight` | CO2 sensor (model confirmed at board bring-up) |
| `pms_a` | `PMS5003` | Single particulate matter sensor |
| `pms_b` | `nullptr` | No second PM sensor on AGo |
| `tvoc_nox` | `SGP41` | TVOC + NOx sensor |
| `o3_no2` | `nullptr` | No AlphaSense electrodes on AGo |
Battery management is handled separately by `PowerService` via the
`airgradient-bms` component and is not part of the `Sensors` struct.
Battery data comes from `PowerService::poll_bms()`, not from `SensorManager`.

## Configuration

`SensorProducer::Config` fields:

| Field | Default | Notes |
|---|---|---|
| `task_stack_size` | `4096` | FreeRTOS task stack in bytes; tune at integration time |
| `task_priority` | `5` | Below input task; above idle |

## Usage

```cpp
#include "go_sensor_producer.h"
#include "services/sensor_manager.h"

// Sensor drivers and Sensors struct owned by the wiring layer.
Sensors sensors{};
sensors.temp_hum = &sht40_driver;
sensors.co2      = &co2_driver;
sensors.pms_a    = &pms5003_driver;
sensors.tvoc_nox = &sgp41_driver;
// pms_b, o3_no2 remain nullptr

SensorManager sensor_manager(sensors);

SensorProducer::Config cfg{};  // defaults: stack=4096, priority=5
SensorProducer sensor_producer(sensor_manager, event_queue, cfg);
sensor_producer.start();

// Orchestrator triggers a measurement cycle (e.g. on MeasurementTimer event):
// iterations = max(1, interval_seconds * 1000 / CONFIG_AVERAGING_ITERATION_INTERVAL_MS)
sensor_producer.request_measurement(30);  // 30 × 2 s = 60 s measurement

// Clean shutdown before deep sleep.
sensor_producer.stop();
```

## Event Output

`SensorProducer` posts `EventType::SensorDataReady` to the orchestrator queue
once per measurement cycle, after `start_measures()` returns.

```cpp
// Event union member (go_events.h):
Measures sensor_data;   // all averaged fields; null-sensor fields carry sentinel values
```

The orchestrator receives this event and routes the `Measures` payload to
storage, display, and BLE services as appropriate.

## Iteration Count

The orchestrator computes the iteration count when it fires `MeasurementTimer`:

```
iterations = max(1, measurement_interval_seconds * 1000
                    / CONFIG_AVERAGING_ITERATION_INTERVAL_MS)
```

`CONFIG_AVERAGING_ITERATION_INTERVAL_MS` defaults to 2000 ms (defined in
`sensor_manager.cpp`).

| Scenario | Iterations |
|---|---|
| Normal measurement cycle (60 s interval) | 30 (60 × 1000 / 2000) |
| First measurement on boot | 1 (get a reading quickly) |
| Low battery mode (future) | 1 (reduce active time) |

The task enforces a minimum of 1 iteration, guarding against an accidental
zero passed by the orchestrator.

## Internal Architecture

### Task Loop

```
SensorProducer::run():
  while _running:
    iterations = 0
    xTaskNotifyWait(0, 0, &iterations, portMAX_DELAY)   // block indefinitely

    if !_running:
      break                                              // stop() was called

    if iterations == 0:
      iterations = 1                                     // zero-iteration guard

    // Blocking: takes (iterations × CONFIG_AVERAGING_ITERATION_INTERVAL_MS)
    measures = SensorManager::start_measures(iterations)

    event{} = { type: SensorDataReady, sensor_data: measures }
    RTOS::queue_send(event_queue, &event, 0)             // non-blocking, drop if full
```

### Orchestrator Signalling

```cpp
// Orchestrator calls this when MeasurementTimer fires:
sensor_producer.request_measurement(iterations);

// Internally:
xTaskNotify(_task_handle,
            static_cast<uint32_t>(iterations),
            eSetValueWithOverwrite);  // overwrites any unconsumed notification
```

`eSetValueWithOverwrite` is used so that if the orchestrator fires two timers
in quick succession (should not occur in normal operation), the task always
sees the most recent iteration count.

### Task Duration

The sensor task is blocked for the full measurement window:
`iterations × CONFIG_AVERAGING_ITERATION_INTERVAL_MS`.  During this time the
orchestrator continues processing other events (GPS fixes, button presses,
timers) uninterrupted.  This is the primary reason for the independent task —
`SensorManager::start_measures()` is inherently blocking.

## stop() Behaviour

`stop()` sets `_running = false`, then calls `vTaskDelete(_task_handle)`.
This deletes the task regardless of whether it is currently blocked on
`xTaskNotifyWait` or executing inside `start_measures()`.  The delete is safe
because `SensorManager` holds no mutexes, so there is no risk of deadlock or
resource leak.

After `stop()` returns, `_task_handle` is `nullptr`.  A subsequent call to
`start()` creates a fresh task.

## Initialization Order

The wiring layer must follow this sequence:

1. Initialise I2C/UART buses.
2. Construct concrete driver instances (`SHT40`, `S8`/`Sunlight`, `PMS5003`, `SGP41`).
3. Call `driver.init()` on each.
4. Populate a `Sensors` struct (unused pointers set to `nullptr`).
5. Construct `SensorManager(sensors)`.
6. Construct `SensorProducer(sensor_manager, event_queue, config)`.
7. Call `sensor_producer.start()`.

## Thread Safety

`SensorProducer` has no shared mutable state accessed from multiple threads
other than:

- `_running` (`volatile bool`): written by the orchestrator via `stop()` and
  read by the sensor task.  No mutex needed — the flag is used only to signal
  exit; the task is deleted immediately after by `vTaskDelete`.
- `_task_handle`: written by `start()` / `stop()` (orchestrator thread) and
  read by `request_measurement()` (also orchestrator thread).  Both callers
  are in the same thread, so no protection is required.

## Testability

FreeRTOS task notification calls (`xTaskNotify`, `xTaskNotifyWait`,
`vTaskDelete`) are guarded by `#ifndef TEST_HOST`, allowing the file to compile
in native host test builds.

For host testing:

- Inject a mock `SensorManager` (or construct one with mock sensor HALs via
  the `Sensors` struct pattern).
- Replace the FreeRTOS queue with a test double or inspect the `Event` struct
  directly.
- `start()` is a no-op in `TEST_HOST` mode (`RTOS::task_create` returns
  `false`); call `run()` directly in tests to exercise the task loop.
- Verify `request_measurement(0)` causes the task to use 1 iteration.
- Verify `SensorDataReady` is posted with the correct `Measures` payload.

## Dependencies

- `airgradient-sensors` — `SensorManager`, `Sensors` struct, sensor HAL interfaces.
- `airgradient-common` — `Measures` types, `RTOS` abstraction.
- `go_events.h` / `go_types.h` — event type and payload definitions.
