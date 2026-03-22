# UI Manager

Product-specific UI state machine for AirGradient Go. Manages screen
navigation, menu/list selection, metric browsing, snackbar notifications,
and chart data extraction.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_ui.h` | `UIManager` class, `UIAction`, `BuildContext` |
| `products/go/main/go_ui.cpp` | Screen state machine, input dispatch, row population, chart extraction |

## Architecture

The UI Manager is a **pure state machine** with zero hardware or RTOS
dependencies. It never touches the event queue or the Display Service
directly. The orchestrator drives it:

```
Orchestrator:
  on InputPress (unlocked):
      action = ui_manager.handle_input(source, type)
      if action: post event

  on display update:
      ui_manager.clear_expired_snackbar(now_ms)
      values = ui_manager.build_values(ctx)
      display_service.update(values)
```

`BuildContext` passes all external state (sensors, GPS, battery, flags,
measurement cache). The UI Manager reads from it but never stores
references to services.

## Public API

| Method | Purpose |
|---|---|
| `handle_input(source, type)` | Process touch input. Returns `UIActionResult` if an app-level state change occurred. |
| `build_values(ctx)` | Build a `DisplayValues` snapshot for the Display Service. |
| `set_screen(screen)` | Force screen (Shutdown, deep-sleep restore). |
| `current_screen()` | Read current screen. |
| `show_snackbar(text)` | Show a 3-second snackbar message. |
| `clear_expired_snackbar(now_ms)` | Expire stale snackbar. Call before `build_values`. |
| `reset_to_home()` | Reset to Home with no metric. Used on auto-lock. |

## UIAction Events

`handle_input()` returns a `UIActionResult`. The `action` field tells the
orchestrator what happened:

| UIAction | Trigger | Notes |
|---|---|---|
| `StartTracking` | Menu: "Start Tracking" | |
| `StopTracking` | Menu: "Stop Tracking" | |
| `ChangeMode` | Settings: Mode choice | `new_mode` field set |
| `SettingsChanged` | Settings: any other choice | |
| `ClearData` | Confirm: "Yes" | |
| `SaveTag` | TagList: tag selected | `tag_index` field set |

## Screen Navigation

```
Home ──enter──> MainMenu
  ^               │
  │ Exit          ├──> Settings ──> SettingsChoice
  │               │       │──> Confirm
  │               ├──> TagList
  │               └──> About
  │                         │
  └─── Exit from any screen─┘
```

Every screen has Exit (index 0) -> Home. Screens with a parent have Back
(index 1) -> parent.

## Navigation Patterns

| Screen | Style | Wrapping | Scroll |
|---|---|---|---|
| Home (metrics) | Circular cycle | Yes | N/A |
| MainMenu | Circular + skip disabled | Yes | N/A |
| Settings | Clamped | No | Page-based (7 items) |
| SettingsChoice | Circular | Yes | Sliding window (7 items) |
| TagList | Clamped | No | Page-based (7 items) |
| About | Circular | Yes | N/A (2 items) |
| Confirm | Circular | Yes | N/A (5 items, index 2 non-selectable) |

## Internal Settings State

The UI Manager stores settings as option indices internally. These drive
the settings row labels and pre-select the current value when opening a
SettingsChoice screen.

**TODO**: Wire to `GoSettings` once it has all required fields
(`use_fahrenheit`, `pm_use_usaqi`, `pm_interval`, `other_sensor_interval`,
`gps_mode` enum). The orchestrator should sync initial values after loading
persisted settings.

## Snackbar Lifecycle

1. `show_snackbar(text)` copies text, marks deadline as pending
2. `clear_expired_snackbar(now_ms)` arms the 3-second deadline on first
   call, then clears when expired
3. `build_values()` sets `v.snackbar_text` if the snackbar is active

## Chart Data

`build_values()` extracts per-metric float values from the `Measures` cache
array (passed via `BuildContext`). Invalid sentinel values are skipped.
Integer fields (CO2, TVOC, NOx) are cast to float. The chart buffer is
sized to `UI_CHART_BUF_SIZE` (16, matching `PAYLOAD_CACHE_MAX_SIZE`).

## Dependencies

- `go_display.h`: `Screen`, `Metric`, `DisplayValues`, `ListRow`, `MAX_LIST_ROWS`
- `go_types.h`: `InputSource`, `InputType`, `OperatingMode`
- `measures_types.h`: `Measures`, `MeasuresInvalid` sentinels

No FreeRTOS. No ESP-IDF. Fully testable on host.
