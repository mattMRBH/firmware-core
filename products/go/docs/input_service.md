# Input Service

Independent RTOS task that handles all user input for AirGradient Go: 3
capacitive touch pads (via CAP1203) and 2 physical buttons (via GPIO).
Classifies raw hardware events into typed `InputPress` events and posts them
to the orchestrator event queue.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_input.h` | `InputService` class declaration, `Config` struct |
| `products/go/main/go_input.cpp` | ISR handlers, task loop, debounce/classification logic |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `CapTouchSensor` (CAP1203) | `airgradient-touch` | Read touch pad state, clear interrupt |
| `gpio::Hal` | `airgradient-gpio` | GPIO pin configuration, ISR registration, level reads |
| `go_types.h` | product | `InputSource`, `InputType` enums |
| `go_events.h` | product | `Event`, `EventType::InputPress` |
| `RTOS` | `airgradient-common` | `task_create`, `task_delete`, `queue_send`, `get_time_ms` |
| RTOS queue | `airgradient-common` | Internal raw queue (`queue_create` / `queue_receive` / `queue_send_from_isr`) |

## Hardware Inputs

| Input | Source | Mapping |
|---|---|---|
| Touch Up | CAP1203 CH1 | `InputSource::TouchUp`, short press only |
| Touch Down | CAP1203 CH2 | `InputSource::TouchDown`, short press only |
| Touch Enter | CAP1203 CH3 | `InputSource::TouchEnter`, short press only |
| Button Power | Physical GPIO | `InputSource::ButtonPower`, short or long press |
| Button Boot | Physical GPIO | `InputSource::ButtonBoot`, short or long press |

All three touch channels share a single INT line. The CAP1203 INT fires when
any enabled channel detects a touch. The task reads the CAP1203 registers in
task context to determine which channel(s) fired.

Physical buttons are active-low (pulled high internally; pressed = GPIO low).

## Configuration

`InputService::Config` fields — all pin assignments come from wiring at
construction time (typically from `board_config.h`):

| Field | Default | Notes |
|---|---|---|
| `pin_cap_int` | — | CAP1203 ALERT/INT output pin (GPIO number) |
| `pin_button_power` | — | Power / lock / unlock physical button |
| `pin_button_boot` | — | Boot / factory-reset physical button |
| `debounce_ms` | `50` | Minimum ms between two accepted press events for the same button |
| `long_press_ms` | `2000` | Duration (ms) a button must be held before firing `LongPress` |
| `task_stack_size` | `3072` | RTOS task stack in words; tune at integration time |
| `task_priority` | `6` | Above GPS task; at or above sensor task |

## Usage

```cpp
#include "go_input.h"
#include "board_config.h"

// Dependencies must outlive the service.
CAP1203 cap1203(i2c_bus);
cap1203.init();

InputService::Config cfg{};
cfg.pin_cap_int      = BOARD_PIN_CAP_INT;
cfg.pin_button_power = BOARD_PIN_BUTTON_POWER;
cfg.pin_button_boot  = BOARD_PIN_BUTTON_BOOT;
// debounce_ms and long_press_ms use defaults (50 / 2000)

InputService input_svc(cap1203, gpio::native::hal, event_queue, cfg);
input_svc.start();

// To shut down cleanly:
input_svc.stop();
```

## Event Output

All events are posted to the orchestrator queue as `EventType::InputPress`
with an `InputEventData` payload:

```cpp
struct InputEventData {
  InputSource source;  // TouchUp / TouchDown / TouchEnter / ButtonPower / ButtonBoot
  InputType   type;    // ShortPress / LongPress
};
```

Touch pads only ever produce `ShortPress`. Physical buttons produce either
`ShortPress` or `LongPress` depending on hold duration.

## Internal Architecture

### ISR → Task Pipeline

```
GPIO interrupt (falling edge)
  |
  v
Static ISR handler (cap_int_isr / button_power_isr / button_boot_isr)
  • Minimal: no heap, no blocking, no logging
  • Posts RawInputEvent{source, 0} via RTOS ISR-safe queue send
  |
  v
Internal raw queue (depth 8, DRAM)
  |
  v
InputService task (input_task)
  • Records timestamp via RTOS::get_time_ms() at dequeue time
  • Routes to process_touch_interrupt() or process_button_event()
  |
  v
Orchestrator event queue  (EventType::InputPress)
```

### Touch Interrupt Processing

1. Call `CapTouchSensor::read()` to get `TouchData` (touched + noise bitmasks).
2. Call `CapTouchSensor::clear_interrupt()` to de-assert the CAP1203 INT line.
3. Compute `valid_touches = touched & ~noise` to discard noisy channels.
4. If any channel has a noise flag, attempt recalibration via `calibrate(noise)`.
5. Map each valid channel bit to its `InputSource` and post `ShortPress`.

The touch-sentinel approach (ISR always posts `TouchEnter` as a placeholder)
keeps ISR code minimal: actual channel determination happens in task context
where I2C reads are safe.

### Physical Button Processing

Debounce and long-press detection run entirely in task context:

1. **Debounce**: Accept only if `now - last_event_time >= debounce_ms` (50 ms).
2. **Arm timer**: Record `press_start_time = now`, set `pending_long_press = true`.
3. **Dynamic timeout**: RTOS queue receive uses a timeout equal to the remaining
   time until the nearest pending long-press expires, so the task wakes up
   exactly when needed.
4. **Check expiry** (`check_pending_long_press`): On each loop iteration
   (after receive or timeout), if `now - press_start >= long_press_ms`:
   - Read GPIO level: if still low (held) → `LongPress`
   - If high (released) → `ShortPress`

This polling approach avoids double-interrupt configuration (no rising-edge ISR
needed for release detection).

## CAP1203 Noise Handling

The CAP1203 reports per-channel noise flags in `TouchData.noise`. Channels with
noise flags set are masked out before posting events:

```cpp
uint8_t valid_touches = data.touched & ~data.noise;
```

When noise is non-zero and the sensor supports calibration, the service
triggers hardware recalibration on the affected channels:

```cpp
if (data.noise != 0 && _touch.supports_calibration()) {
    _touch.calibrate(data.noise);
}
```

## `CapTouchSensor::clear_interrupt()` — HAL Extension

The `clear_interrupt()` method was added to `CapTouchSensor` (base class) as a
virtual no-op (`return true`) so `InputService` can call it through the HAL
interface without requiring a concrete `CAP1203` reference. `CAP1203` overrides
it to read `MAIN_CONTROL` (clears INT bit) and `GENERAL_STATUS` (latches
touch-status flags).

## Deep Sleep

Physical button GPIOs are configured as deep sleep wake sources by the Power
Management service before entering sleep. The Input Service does not manage
sleep wake configuration.

On wake from button press, the first button event is handled as a
`WakeFromSleep(Button)` event by the orchestrator, not as an `InputPress`. The
Input Service starts normally afterward and processes subsequent presses.

## Testability

- Inject a mock `CapTouchSensor` returning controlled `TouchData`.
- Construct `gpio::Hal` with mock function pointers for level reads and ISR
  registration.
- The debounce and long-press classification logic is in `process_button_event`
  and `check_pending_long_press` — pure functions of timestamps and GPIO level,
   testable without RTOS.
- RTOS queue operations are no-ops under `TEST_HOST` so the
  translation unit compiles cleanly for native host tests.
