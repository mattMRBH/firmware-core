# AirGradient Go (AGo) — Architecture Specification

This document captures the product architecture for the AirGradient Go portable
air quality monitor. It is the output of an initial design brainstorm and serves
as the reference for implementation work.

## 1. Product Overview

AirGradient Go is a portable, battery-powered air quality monitor with GPS
tracking, e-paper display, touch/button navigation, and multiple connectivity
modes. The device measures environmental data (PM, CO2, TVOC, NOx, temperature,
humidity), logs routes with GPS coordinates, and streams data over BLE or serves
it over WiFi.

## 2. Modes and Behaviors

AGo has two orthogonal state dimensions.

### 2.1 Operating Modes (set rarely)

| Mode | Radio | Power Source |
|---|---|---|
| Portable | BLE streams data to phone | Battery |
| Stationary | WiFi with local HTTP server | Battery or USB |
| Offline | No radio | Battery |

### 2.2 Behaviors (change frequently)

| Behavior | Description |
|---|---|
| Tracking | GPS + sensor logging, persist route to NAND flash, temporary cache for chart |
| Idle | Sensor readings on display, temporary cache for chart only |
| Shutdown | BMS QoN — full power off |

All three modes can enter any behavior. Tracking in Offline mode means GPS +
sensor logging with no radio transmission.

## 3. High-Level Architecture

The system uses a **single centralized event queue** with an orchestrator loop.
Producers post typed events into a FreeRTOS queue. The orchestrator consumes
events, updates state machines, and calls consumers directly.

```
+--------------------------------------------------------------+
|                    Orchestrator (main loop)                   |
|                                                              |
|  +------------+  +------------+  +---------------------+     |
|  | App State  |  | UI State   |  | Power Policy        |     |
|  | Mode+Behav |  | Lock/Pages |  | sleep eligibility   |     |
|  +------------+  +------------+  +---------------------+     |
|                                                              |
|  dispatch(event) -> switch on event type                     |
|    -> update state machines                                  |
|    -> call consumers directly                                |
+----------------------------+---------------------------------+
                             |
                    Event Queue (FreeRTOS queue)
                             |
     +-----------+-----------+-----------+-----------+
     |           |           |           |           |
+---------+ +--------+ +--------+ +--------+ +--------+
| Sensor  | |  GPS   | | Input  | |  BMS   | | Timer  |
| Task    | |  Task  | |  Task  | | polled | | system |
+---------+ +--------+ +--------+ +--------+ +--------+
  Producers -- each posts typed events to the queue

             Orchestrator calls directly:
     +-----------+-----------+-----------+-----------+
     |           |           |           |           |
+---------+ +--------+ +--------+ +--------+ +--------+
| Display | |Storage | |  BLE   | | Power  | |  UI   |
| Service | |Service | |Service | |  Mgmt  | |Manager|
+---------+ +--------+ +--------+ +--------+ +--------+
  Consumers -- called by orchestrator, not event-driven
```

### 3.1 Event Flow Direction

- **Events** flow from producers into the orchestrator via the event queue.
- **Commands** flow from the orchestrator to consumers via direct method calls.
- The orchestrator never blocks on a consumer. Slow operations (e-paper refresh)
  are handled asynchronously by the consumer's internal task.

## 4. State Machines

### 4.1 App State Machine

Controls what the device is doing. Two orthogonal dimensions:

```
Operating Mode:     Portable | Stationary | Offline
Behavior:           Tracking | Idle | Shutdown
```

Mode affects which radios/features are enabled. Behavior affects the
measurement + logging pipeline. These are independent — the device can be in
any (mode, behavior) combination.

Transitions:
- Mode changes: user navigates UI menu
- Tracking start/stop: user navigates UI menu
- Shutdown: physical button long press (Button 1) -> BMS QoN

### 4.2 UI State Machine

Controls what is on the display. Independent from the app state machine, though
UI actions can trigger app state transitions (e.g., user selects "Start
Tracking" in a menu).

```
Screens:  Dashboard | Menu | Settings | RouteHistory | ...
```

The UI state machine consumes input events (touch/button) and decides what to
render. The orchestrator forwards input events to the UI manager when the device
is unlocked.

### 4.3 Lock / Unlock

A lock/unlock gate separates interactive and passive display modes.

| State | Touch Pads | Display | Sleep | Button 1 Short |
|---|---|---|---|---|
| Locked | Ignored | Static dashboard (periodic refresh) | Eligible | Unlock |
| Unlocked | Active (Up/Down/Enter) | Interactive menus | Never | Lock |

Both states: Button 1 long press -> Shutdown.

Inactivity timeout while unlocked -> auto-lock. The timeout value is
configurable via settings (minimum 5 seconds).

While locked, the display periodically updates sensor values and status on the
dashboard. The refresh interval is configurable and can be disabled entirely.

## 5. Event Types

### 5.1 Producer Events (into queue)

| Event | Source | Payload |
|---|---|---|
| `SensorDataReady` | Sensor Task | `Measures` struct |
| `GpsFixUpdate` | GPS Task | `GpsData` from `airgradient-gps` — position, altitude, fix type, DOP, satellite count, timestamp |
| `InputPress` | Input Task | source (touch_up/down/enter, btn_power, btn_boot), type (short/long) |
| `BatteryStatus` | Orchestrator (polled) | voltage, charge percent, charging state, critical flag |

### 5.2 System Events (into queue)

| Event | Source | Meaning |
|---|---|---|
| `InactivityTimeout` | Timer | No input for configured duration -> auto-lock |
| `MeasurementTimer` | Timer | Time to start next measurement cycle |
| `WakeFromSleep` | Boot path | Wake cause: timer or button |

### 5.3 UI Action Events (UI -> orchestrator, into queue)

| Event | Meaning |
|---|---|
| `UserStartTracking` | User selected start tracking in menu |
| `UserStopTracking` | User selected stop tracking in menu |
| `UserChangeMode` | User selected Portable/Stationary/Offline |
| `UserChangeSetting` | User changed a setting (key + value) |
| `UserToggleGps` | User enabled/disabled GPS in software |

## 6. Tasks

### 6.1 Task Map

| Task | Role | Blocks? | Posts to Queue | Notes |
|---|---|---|---|---|
| Orchestrator | Main event loop | No | -- | Runs in `app_main` context or dedicated task |
| Sensor Producer | Wraps SensorManager | Yes | `SensorDataReady` | Waits for task notification from orchestrator |
| GPS Producer | UART NMEA read loop | Yes | `GpsFixUpdate` | Product-specific, uses `airgradient-gps` (`GpsSensor` / `NmeaGps`) |
| Input Producer | Classifies raw ISR events | Yes | `InputPress` | Debounce + long-press detection |
| Display Worker | Drives e-paper refresh | Yes | -- | Receives render commands from orchestrator |

BMS is polled directly by the orchestrator on a timer (I2C read, fast and
non-blocking). No dedicated task.

### 6.2 Sensor Producer

Wraps the shared `SensorManager` from `components/airgradient-sensors/`:

```
Sensor Task:
  loop:
    Wait for task notification from orchestrator
    Call SensorManager::start_measures(iterations)   // blocking
    Post Event::SensorDataReady to event queue
```

The orchestrator controls when to measure by sending a task notification. The
number of iterations can vary based on current behavior and measurement interval
settings.

### 6.3 GPS Producer

Product-specific `GpsService` task in `products/go/main/`. Uses the shared
`airgradient-gps` component for all NMEA parsing and serial I/O. The service
holds a `GpsSensor &` reference (concrete type: `NmeaGps`) and calls its
non-blocking `read()` method in a loop. NMEA sentence accumulation, checksum
validation, and field extraction are fully handled by the component.

```
GPS Task:
  loop:
    Call GpsSensor::read()            // drains serial buffer, parses sentences
    If read() returned true:
      Update _latest_fix from GpsSensor::get_data()
      If timestamp valid and clock not yet synced:
        Set ESP32 system clock via settimeofday()
    If posting interval elapsed and GpsSensor::has_valid_fix():
      Post Event::GpsFixUpdate to event queue
    Delay 10 ms                       // yield, avoid busy-wait
```

`GpsData` (from `airgradient-gps/types/gps_types.h`) carries position
(`GpsPosition`), altitude, fix metadata (`GpsFix` — type, satellite count,
DOP values), and a UTC timestamp (`GpsTimestamp`). The orchestrator can also
call `GpsService::get_latest_fix()` directly at any time (mutex-protected).

GPS hardware is always powered on (no software on/off control at hardware
level). The software enable/disable setting controls whether the orchestrator
uses GPS data, not whether the task runs. When GPS is disabled in settings, the
orchestrator ignores `GpsFixUpdate` events.

GPS data gaps during deep sleep are acceptable. The GPS module maintains its fix
independently; the ESP reads the current position immediately on wake.

### 6.4 Input Producer

Handles all 5 inputs: 3 capacitive touch pads (via `airgradient-touch`) and 2
physical buttons (via `airgradient-gpio`).

```
ISR (GPIO/Touch interrupt):
  Post raw event (gpio_id + timestamp) to raw_input_queue via xQueueSendFromISR

Input Task:
  loop:
    Receive raw event from raw_input_queue
    Debounce
    Classify: short press vs long press (physical buttons only)
    Post Event::InputPress to orchestrator event queue
```

Touch pads support short press only. Physical buttons support short and long
press.

Input mapping:

| Input | Type | Role |
|---|---|---|
| Touch Pad 1 | Capacitive | Navigate Up |
| Touch Pad 2 | Capacitive | Navigate Down |
| Touch Pad 3 | Capacitive | Enter / Select |
| Button 1 | Physical | Short: lock/unlock. Long: shutdown |
| Button Boot | Physical | Short: unused. Long: factory reset |

### 6.5 Display Worker

E-paper display with an independent task for the slow SPI refresh.

```
Orchestrator:
  display_service.update(current_screen_data)   // prepares framebuffer
  // returns immediately

Display Task:
  loop:
    Wait for "frame ready" signal
    Drive e-paper refresh (slow, blocking SPI)
```

The orchestrator never blocks on e-paper refresh. The display service API is
synchronous (prepare what to render), but the hardware refresh is async.

## 7. Power Management

### 7.1 Sleep Eligibility

Sleep is only eligible when the device is **locked**. When unlocked (user
interacting with menus), the device never sleeps.

### 7.2 Sleep Type Selection

The orchestrator calculates the next wake time:

```
next_wake = min(measurement_interval, display_refresh_interval)
// if display refresh is disabled, only measurement_interval matters
```

| Interval | Sleep Type | Rationale |
|---|---|---|
| >= ~5 seconds | Deep sleep | Worth the reboot overhead |
| < ~5 seconds | Light sleep | Deep sleep boot cost too high |

The exact threshold is a tunable constant.

### 7.3 Sleep Entry

```
1. Calculate next wake time
2. Configure wake sources:
   - Timer: next wake time
   - GPIO: Button Power (unlock), Button Boot (factory reset)
3. Persist app state to RTC memory:
   - Current mode, behavior, lock status
   - Tracking-in-progress flag
   - GPS enabled/disabled
4. Enter deep sleep (or light sleep)
```

### 7.4 Wake and Boot Path

Deep sleep reboots the CPU. All tasks restart from `app_main`. The boot path
includes a **fast-path** for timer wakes to avoid spinning up the full event
loop unnecessarily.

```
app_main:
  1. Check wake cause (timer vs button vs fresh power-on)
  2. Restore app state from RTC memory

  If fresh power-on:
    Full initialization -> Locked + Idle -> enter event loop

  If timer wake (locked):
    FAST PATH:
      - Initialize only sensor + display (minimal)
      - Run one measurement cycle
      - If tracking: read GPS, log data point
      - Update display
      - Re-enter sleep
      (Never starts the full event loop)

  If button wake:
    Full initialization -> restore previous state -> Unlock -> enter event loop
```

The fast-path avoids starting GPS task, input task, and the full orchestrator
for what is essentially a "measure and sleep" cycle.

### 7.5 Shutdown

Button 1 long press triggers BMS QoN (ship mode). The device fully powers off.
GPS module loses power. Next power-on is a fresh boot.

## 8. Services (products/go/main/)

These are the product-specific service modules that live in the AGo product
root. Each service is a focused unit that the orchestrator wires together.

### 8.1 GPS Service

- Product-specific service (`go_gps.h` / `go_gps.cpp`) — not a shared component
- Delegates all NMEA parsing and serial I/O to the shared `airgradient-gps`
  component; holds a `GpsSensor &` reference (concrete type: `NmeaGps`)
- `GpsData` type comes from `airgradient-gps/types/gps_types.h`; no separate
  product-local GPS data struct
- Independent task (GPS Producer); clean shutdown via `stop()` which blocks
  until the task self-signals before deleting itself
- API: `start()`, `stop()`, `get_latest_fix()` (mutex-protected), `set_posting_interval_ms()`
- Posts `GpsFixUpdate` events to the orchestrator queue at the configured interval
- Syncs ESP32 system clock (`settimeofday`) on the first valid GPS timestamp
- `gps_read_once()` free function provides a synchronous one-shot read for the
  fast-path timer-wake boot path, without starting the full task infrastructure

### 8.2 Input Service (Button Service)

- Uses `airgradient-touch` (CAP1203) for 3 capacitive touch pads
- Uses `airgradient-gpio` for 2 physical buttons with interrupt
- Independent task (Input Producer)
- Handles ISR -> raw queue -> debounce -> classify -> typed event
- Posts `InputPress` events to the orchestrator queue
- Future-proof: additional input sources (IMU) can post the same event types

### 8.3 Storage Service

Two tiers of storage:

**Temporary (chart data):**
- Uses `airgradient-payload-cache` with `RtcPayloadCacheStorage` backend
- Keeps last N measurements for display chart rendering
- Survives deep sleep (RTC memory), lost on power-off
- Ring buffer semantics (overwrites oldest when full)

**Persistent (route data):**
- Uses `airgradient-nand-storage` (SPI NAND with FATFS)
- Stores GPS + sensor data for tracking routes
- Survives power-off
- POSIX file I/O on mounted filesystem
- Route data format, retention policy, and retrieval mechanism TBD

### 8.4 Power Management

- BMS chip interaction (status reads, QoN shutdown command)
- Uses existing BMS driver from `airgradient-sensors`
- Polled by orchestrator on a timer (no dedicated task)
- Sleep cycle management (deep/light sleep entry, wake source config)
- RTC memory state persistence before sleep
- Fast-path boot logic for timer wakes

### 8.5 Settings

- Uses `airgradient-config` (`ConfigStore` with NVS backend)
- Follows reference product pattern (`ReferenceSettings`)
- Product-specific settings struct with field validation

Known settings:
- Measurement interval
- Display refresh interval (can be disabled)
- Inactivity timeout (min 5 seconds)
- GPS enabled/disabled
- Operating mode (Portable/Stationary/Offline)
- Device name (for BLE/WiFi)

### 8.6 Display Service (E-Paper)

- Independent worker task for e-paper SPI refresh
- Dashboard page: sensor values, GPS status, battery, tracking indicator,
  lock indicator
- Menu system: navigated via touch pads (Up/Down/Enter)
- Orchestrator calls display service API (non-blocking), display task handles
  hardware refresh asynchronously
- Implementation deferred

### 8.7 BLE Streams

- BLE peripheral mode using `airgradient-ble` (NimBLE)
- Streams sensor + GPS data to connected phone
- Only active in Portable mode
- Implementation deferred

## 9. Data Flow Examples

### 9.1 Locked + Tracking + Measurement Timer Fires

```
1. Timer posts MeasurementTimer to queue
2. Orchestrator receives event
3. Orchestrator sends task notification to Sensor Producer
4. Sensor Producer calls SensorManager::start_measures(N)
5. Sensor Producer posts SensorDataReady(measures) to queue
6. Orchestrator receives SensorDataReady
7. Orchestrator reads latest GPS fix (from last GpsFixUpdate)
8. Orchestrator calls Storage::persist_route_point(measures, gps_fix)
9. Orchestrator calls Storage::cache_temporary(measures)
10. Orchestrator calls Display::update_dashboard(measures, gps, battery)
11. If Portable mode: Orchestrator calls BLE::stream(measures, gps)
12. Orchestrator evaluates sleep eligibility -> locked -> sleep
```

### 9.2 User Unlocks Device

```
1. Button 1 GPIO interrupt fires
2. ISR posts raw event to Input Task raw queue
3. Input Task classifies: Button 1, short press
4. Input Task posts InputPress(btn_power, short) to orchestrator queue
5. Orchestrator receives InputPress
6. Orchestrator checks: currently locked -> transition to unlocked
7. Orchestrator disables sleep eligibility
8. Orchestrator resets inactivity timer
9. Orchestrator calls Display::show_dashboard_interactive()
10. Display shows unlock indicator
```

### 9.3 Deep Sleep Fast-Path (Timer Wake)

```
1. ESP32 wakes from deep sleep (timer)
2. app_main reads wake cause: timer
3. app_main restores state from RTC: locked, tracking, mode
4. FAST PATH: no full event loop
5. Initialize sensor bus + SensorManager
6. Call SensorManager::start_measures(1)  // single iteration
7. If tracking: call gps_read_once(GpsSensor, baud, timeout) for a one-shot fix
8. Persist route point + cache temporary
9. Update e-paper display
10. Re-enter deep sleep
```

## 10. Implementation Specs

Detailed implementation specifications for each service:

- [Common Types](specs/common_types.md) — events, shared structs, data flow types
- [Settings Service](specs/settings.md)
- [Sensor Producer](specs/sensor_producer.md)
- [GPS Service](specs/gps_service.md)
- [Input Service](specs/input_service.md)
- [Storage Service](specs/storage_service.md)
- [Display Service](specs/display_service.md)
- [UI Manager](specs/ui_manager.md)
- [Power Management](specs/power_management.md)
