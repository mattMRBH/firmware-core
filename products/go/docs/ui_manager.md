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
| `show_pairing_passkey(passkey)` | Show 6-digit BLE passkey on dedicated screen. |
| `dismiss_pairing_passkey()` | Dismiss passkey screen, return to Home. |
| `apply_to_settings(settings)` | Convert internal option indices back to `GoSettings` field values. Reverse of `sync_settings`. |

## UIAction Events

`handle_input()` returns a `UIActionResult`. The `action` field tells the
orchestrator what happened:

| UIAction | Trigger | Notes |
|---|---|---|
| `StartTracking` | Menu: "Start Tracking" | |
| `StopTracking` | Menu: "Stop Tracking" | |
| `ChangeMode` | Settings: Mode choice | `new_mode` field set |
| `SettingsChanged` | Settings: any other choice | |
| `ClearData` | Confirm: "Yes" (from "Data: Clear Data") | |
| `CalibrateCo2` | Confirm: "Yes" (from "CO2: Calibrate") | |
| `SaveTag` | TagList: tag selected | `tag_index` + `tag_label` fields set (plumbing preserved, menu entry removed) |

Opening the main menu resets the active metric to `None`, clearing any
hero/grid selection highlight behind the overlay.

The UI Manager does **not** show snackbars for action-producing dispatches.
The orchestrator owns all user feedback because it executes the actual
operation and knows whether it succeeded (e.g., `clear_data()` may report
partial failure).

## Screen Navigation

```
Home ──enter──> MainMenu
  ^               │
  │ Exit          ├──> Settings ──> SettingsChoice
  │               │       │──> Confirm
  │               └──> About
  │                         │
  └─── Exit from any screen─┘

Externally set (not user-navigable):
  Shutdown         ── set by orchestrator on long-press power
  PairingPasskey   ── set by orchestrator on BLE pairing request
```

MainMenu rows: Exit Menu (0), Start/Stop Tracking (1), Settings (2),
About Device (3). "Add Tag" has been removed from the menu; tag list
plumbing (`dispatch_tag_list`, `open_tag_list`, `SaveTag`) is preserved
but not reachable from the menu.

Every screen has Exit (index 0) -> Home. Screens with a parent have Back
(index 1) -> parent. Shutdown and PairingPasskey are set directly by the
orchestrator via `set_screen()` / `show_pairing_passkey()` and do not
accept user input.

The Confirm screen is a shared confirmation dialog used by multiple
settings actions ("CO2: Calibrate" and "Data: Clear Data"). The question
text and the resulting `UIAction` are determined by `_confirm_source_setting`,
which records which setting row opened the dialog. Back and No both return
to the Settings screen with the cursor on the source row.

## Navigation Patterns

| Screen | Style | Wrapping | Scroll |
|---|---|---|---|
| Home (metrics) | Circular cycle (5 entries: None, Pm25, Co2, Temp, Humidity) | Yes | N/A |
| MainMenu | Circular (4 rows, all always enabled) | Yes | N/A |
| Settings | Clamped | No | Page-based (8 items) |
| SettingsChoice | Circular | Yes | Sliding window (8 items) |
| TagList | Clamped | No | Page-based (8 items) |
| About | Circular | Yes | N/A (2 items) |
| Confirm | Circular | Yes | N/A (5 items, index 2 non-selectable) |
| Shutdown | N/A (no input) | N/A | N/A |
| PairingPasskey | N/A (no input) | N/A | N/A |

## Internal Settings State

The UI Manager stores settings as option indices internally. These drive
the settings row labels and pre-select the current value when opening a
SettingsChoice screen.

The orchestrator calls `sync_settings(const GoSettings &)` after loading
persisted settings from NVS to synchronize the internal option indices.

## Snackbar Lifecycle

1. `show_snackbar(text)` copies text, marks deadline as pending
2. `clear_expired_snackbar(now_ms)` arms the 3-second deadline on first
   call, then clears when expired
3. `build_values()` sets `v.snackbar_text` if the snackbar is active

## Chart Data

`build_values()` extracts per-metric float values from the `MeasuresAGo` cache
array (passed via `BuildContext`). Invalid sentinel values are skipped.
Integer fields (CO2, TVOC, NOx) are cast to float. The chart buffer is
sized to `UI_CHART_BUF_SIZE` (16, matching `PAYLOAD_CACHE_MAX_SIZE`).

## Dependencies

- `go_display.h`: `Screen`, `Metric`, `DisplayValues`, `ListRow`, `MAX_LIST_ROWS`
- `go_types.h`: `InputSource`, `InputType`, `OperatingMode`
- `measures_types.h`: `MeasuresAGo`, `MeasuresInvalid` sentinels
- `go_settings.h`: `GoSettings` (for `sync_settings()`)

No RTOS. No ESP-IDF. Fully testable on host.
