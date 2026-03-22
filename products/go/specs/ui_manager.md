# UI Manager — Implementation Spec

Product-specific UI state machine for AirGradient Go. Manages screen navigation,
menu/list selection, metric browsing, snackbar notifications, and chart data
extraction. The UI Manager is a passive component: the orchestrator drives it by
forwarding input events and requesting render snapshots.

The UI Manager does not interact with hardware. It produces `DisplayValues`
snapshots consumed by the Display Service.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_ui.h` | `UIManager` class declaration |
| `products/go/main/go_ui.cpp` | Screen state machine, input dispatch, menu logic, chart extraction |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `go_display.h` | product | `DisplayValues`, `Screen`, `Metric`, `ListRow` |
| `go_types.h` | product | `InputSource`, `InputType`, `OperatingMode` |
| `go_events.h` | product | `EventType` for UI action events |
| `measures_types.h` | `airgradient-common` | `Measures` struct (for chart extraction) |

No FreeRTOS, no ESP-IDF, no hardware. Fully testable on host.

## Orchestrator Integration

The UI Manager is a consumer called directly by the orchestrator. It does not
own a task, does not read from the event queue, and does not call the Display
Service. The orchestrator is the glue:

```
Orchestrator event loop:

  on InputPress event:
      if locked:
          if input is TouchEnter + LongPress:
              unlock, show snackbar
          else:
              show "Long press Menu 2s to unlock" snackbar
      else:
          action = ui_manager.handle_input(input)
          if action has value: post UI action event to queue
          reset inactivity timer

  on display update trigger (input, timer, state change):
      values = ui_manager.build_values(sensor, gps, battery, flags, cache)
      display_service.update(values)
```

The orchestrator owns lock state and the inactivity timer. The UI Manager
receives lock state as a parameter when building the `DisplayValues` snapshot.

## UIAction

When the user selects a menu item that changes application state, `handle_input`
returns an action the orchestrator should process:

```cpp
enum class UIAction : uint8_t {
    None,
    StartTracking,
    StopTracking,
    ChangeMode,           // accompanied by new_mode field
    SettingsChanged,      // UI Manager already wrote to settings
    ClearData,
    SaveTag,              // accompanied by tag_index field
};

struct UIActionResult {
    UIAction action       = UIAction::None;
    OperatingMode new_mode = OperatingMode::Offline;
    uint8_t tag_index     = 0;
};
```

The orchestrator inspects the returned `UIActionResult` and posts the
appropriate event to the event queue (e.g. `UserStartTracking`).

**EventType mapping**:

| UIAction | EventType | Status |
|---|---|---|
| StartTracking | `UserStartTracking` | Exists in `go_events.h` |
| StopTracking | `UserStopTracking` | Exists |
| ChangeMode | `UserChangeMode` | Exists |
| SettingsChanged | `SettingsChanged` | Exists |
| ClearData | — | **Needs addition** to `go_events.h` |
| SaveTag | — | **Needs addition** to `go_events.h` (with `uint8_t tag_index` payload) |

The `UserToggleGps` event in `go_events.h` uses a `bool` payload but the UI
settings menu exposes a three-state GPS mode (Off / On When Tracking / Always
On). This needs reconciliation: either extend the event to carry a GPS mode
enum, or handle the mapping in the orchestrator.

## Screen State Machine

### Screen Enum

From `go_display.h`:

```
Home, MainMenu, Settings, SettingsChoice, TagList, About, Confirm, Shutdown
```

### Navigation Flow

```
                    +----------+
         +---------+   Home   +<----------------------------------+
         | enter   +----------+  <- up/down: browse metrics       |
         v              ^ [Exit Menu]                             |
    +----------+        |                                         |
    | MainMenu +--------+                                         |
    |          +--- [Start/Stop Tracking] ------------------------+
    |          +--- [Add Tag] --+                          [Exit] |
    |          +--- [Settings]  |                                 |
    |          +--- [About]     |                                 |
    +----------+                |                                 |
         |  |                   v                                 |
         |  |           +----------+                              |
         |  |           | TagList  +-- select tag ----------------+
         |  |           |          +-- [Back] -> MainMenu         |
         |  |           +----------+                              |
         |  |                                                     |
         |  v                                                     |
         | +----------+                                           |
         | | Settings +-- [Back] -> MainMenu                      |
         | |          +-- [Clear Data] --+                        |
         | |          +-- select item    |                        |
         | +----------+       |          |                        |
         |      ^             v          v                        |
         |      |     +---------------+ +----------+              |
         |      |     |SettingsChoice | | Confirm  +-- [Yes] ----+
         |      +-----+ [Back]->Set.  | |          +-- [No] -> Settings
         |            +---------------+ +----------+              |
         v                                                        |
    +----------+                                                  |
    |  About   +-- [Back] -> MainMenu                             |
    |          +-- [Exit] ----------------------------------------+
    +----------+
```

Every screen has an Exit option (index 0) that returns to Home. Screens with a
parent have a Back option (index 1) that returns to the parent.

### Shutdown

The orchestrator sets the screen to `Shutdown` directly (not via the UI
Manager). When the orchestrator enters Shutdown behavior, it calls
`ui_manager.set_screen(Screen::Shutdown)` and then triggers a display update.

## UIManager Class

```cpp
class UIManager {
  public:
    struct Config {
        const char *firmware_version;    // e.g. "0.1.0"
        const char *serial_number;       // 12-char hex string
    };

    explicit UIManager(const Config &config);

    /// Process an input event. Returns an action if the input triggered
    /// an application-level state change (start tracking, change mode, etc.).
    /// The orchestrator should not call this when the device is locked.
    UIActionResult handle_input(InputSource source, InputType type);

    /// Build a complete DisplayValues snapshot for the Display Service.
    /// The orchestrator passes in all external state (sensor data, GPS,
    /// battery, status flags, and the temporary measurement cache).
    DisplayValues build_values(const BuildContext &ctx) const;

    /// Force the screen to a specific value. Used by the orchestrator
    /// for Shutdown and for restoring state after deep sleep wake.
    void set_screen(Screen screen);

    /// Get the current screen (for orchestrator decisions).
    Screen current_screen() const;

    /// Show a snackbar message. Duration: 3 seconds.
    void show_snackbar(const char *text);

    /// Clear snackbar if expired. Called by orchestrator before build_values.
    void clear_expired_snackbar(uint32_t now_ms);

    /// Reset to Home screen with no metric selected. Used on auto-lock.
    void reset_to_home();

  private:
    Config _config;

    // Screen state
    Screen _screen          = Screen::Home;
    Metric _active_metric   = Metric::None;

    // Menu selection indices (per screen)
    uint8_t _menu_index     = 0;
    uint8_t _settings_index = 1;
    uint8_t _settings_choice_index = 1;
    uint8_t _about_index    = 1;
    uint8_t _confirm_index  = 1;
    uint8_t _tag_list_index = 1;

    // Scroll state
    uint8_t _settings_scroll_start = 0;
    uint8_t _settings_choice_scroll_start = 0;
    uint8_t _tag_scroll_start = 0;

    // Active settings choice context
    uint8_t _editing_setting_id = 0;

    // Snackbar
    char _snackbar_text[48]    = {};
    uint32_t _snackbar_deadline_ms = 0;

    // Input dispatch (per screen)
    UIActionResult dispatch_home(InputSource source, InputType type);
    UIActionResult dispatch_menu(InputSource source, InputType type);
    UIActionResult dispatch_settings(InputSource source, InputType type);
    UIActionResult dispatch_settings_choice(InputSource source, InputType type);
    UIActionResult dispatch_about(InputSource source, InputType type);
    UIActionResult dispatch_confirm(InputSource source, InputType type);
    UIActionResult dispatch_tag_list(InputSource source, InputType type);

    // Navigation helpers
    void go_home();
    void open_main_menu();
    void open_settings();
    void open_settings_choice(uint8_t setting_id);
    void open_about();
    void open_tag_list();
    void open_confirm();

    // Row population
    void populate_menu_rows(DisplayValues &v, bool tracking_active) const;
    void populate_settings_rows(DisplayValues &v) const;
    void populate_settings_choice_rows(DisplayValues &v) const;
    void populate_about_rows(DisplayValues &v) const;
    void populate_confirm_rows(DisplayValues &v) const;
    void populate_tag_list_rows(DisplayValues &v) const;

    // Chart extraction
    void populate_chart(DisplayValues &v, const Measures *cache,
                        uint8_t cache_count) const;
};
```

## BuildContext

The orchestrator passes all external state to `build_values()` via a context
struct. This keeps the UI Manager free of service dependencies:

```cpp
struct BuildContext {
    // Latest sensor data
    const Measures &sensor_data;

    // GPS clock
    uint8_t hour;           // 0xFF = no data
    uint8_t minute;

    // Battery
    uint8_t battery_pct;    // 0xFF = no data
    bool is_battery_charging;

    // Status flags (from orchestrator state)
    bool locked;
    bool ble_enabled;
    bool ble_connected;
    bool wifi_enabled;
    bool gps_enabled;
    bool gps_fix;
    bool tracking_active;

    // Settings-derived flags
    bool display_off;
    bool use_fahrenheit;
    bool pm_use_usaqi;

    // Temporary measurement cache (for chart rendering)
    const Measures *cache;
    uint8_t cache_count;

    // Current timestamp for snackbar expiry
    uint32_t now_ms;
};
```

## Input Dispatch

### Input Mapping

Touch pads produce `ShortPress` only. Physical buttons produce `ShortPress`
and `LongPress`. The orchestrator handles lock/unlock and shutdown before
forwarding to the UI Manager.

**Inputs forwarded to UI Manager** (device must be unlocked):

| Input | Action |
|---|---|
| `TouchUp` + `ShortPress` | Navigate up / previous |
| `TouchDown` + `ShortPress` | Navigate down / next |
| `TouchEnter` + `ShortPress` | Select / activate |

**Inputs handled by orchestrator** (not forwarded):

| Input | Action |
|---|---|
| `TouchEnter` + `LongPress` | Toggle lock (any screen) |
| `ButtonPower` + `ShortPress` | Lock/unlock |
| `ButtonPower` + `LongPress` | Shutdown |
| `ButtonBoot` + `LongPress` | Factory reset |

### Per-Screen Behavior

| Screen | Up (TouchUp) | Down (TouchDown) | Enter (TouchEnter) |
|---|---|---|---|
| Home | Browse metric -1 | Browse metric +1 | Open MainMenu |
| MainMenu | Move selection -1 (skip disabled) | Move selection +1 (skip disabled) | Activate item |
| Settings | Move selection -1 | Move selection +1 | Activate item |
| SettingsChoice | Move selection -1 | Move selection +1 | Apply choice |
| TagList | Move selection -1 | Move selection +1 | Select tag |
| About | Move selection -1 | Move selection +1 | Activate (Exit/Back) |
| Confirm | Move selection -1 | Move selection +1 | Activate (Exit/Back/Yes/No) |

## Metric Browsing

On the Home screen, Up/Down cycles through metrics:

```
Cycle order: [None, Pm25, Co2, Temp, Humidity, Tvoc, Nox]
                                              (wraps circularly)
```

- `None` = no metric selected. Bottom row shows Pressure/Altitude. Logo visible.
- Any other = metric area highlighted (inverted). Bottom row shows Min/Max.
  Chart visible.

Browsing is disabled when `display_off` is true.

## Menu Content

### Main Menu

| Index | Label | Notes |
|---|---|---|
| 0 | "Exit Menu" | Go to Home |
| 1 | "Start Tracking" / "Stop Tracking" | Toggle based on `tracking_active` |
| 2 | "Add Tag" | Disabled when not tracking |
| 3 | "Settings" | Open Settings screen |
| 4 | "About Device" | Open About screen |

Default selection on open: index 0 (Exit Menu).

### Settings

| Index | Label | Notes |
|---|---|---|
| 0 | "Exit" | Go to Home |
| 1 | "Back" | Go to MainMenu |
| 2 | "Units: C" / "Units: F" | Temperature unit |
| 3 | "PM Display: ug/m3" / "PM Display: USAQI" | PM display mode |
| 4 | "Display Interval: 10s" | Display refresh rate |
| 5 | "PM Interval: 10s" | PM sensor interval |
| 6 | "Other Sensor Int.: 10s" | Other sensors interval |
| 7 | "GPS Mode: On When Tracking" | GPS behavior |
| 8 | "Mode: Portable" | Operating mode |
| 9 | "Auto Lock: Off" | Auto-lock timeout |
| 10 | "Data: Clear Data" | Opens Confirm dialog |

Default selection on open: index 1 (Back).

### Settings Choices

Each setting opens a SettingsChoice screen:

| Setting | Options |
|---|---|
| Units | "C", "F" |
| PM Display | "ug/m3", "USAQI" |
| Display Interval | "1s", "10s", "30s", "60s", "5m", "15m", "1h", "Display Off" |
| PM Interval | "1s", "10s", "30s", "60s", "5m", "15m", "1h", "Off" |
| Other Sensor Int. | "1s", "10s", "30s", "60s", "5m", "15m", "1h", "Off" |
| GPS Mode | "Always Off", "On When Tracking", "Always On" |
| Mode | "Stationary", "Portable", "Offline / Airplane Mode" |
| Auto Lock | "Off", "10 Seconds", "30 Seconds", "60 Seconds" |

Selecting a choice applies it, writes to settings, and returns to the Settings
screen. The UI Manager writes settings directly via the `GoSettings` reference.

### Tag List

| Index | Label |
|---|---|
| 0 | "Exit" |
| 1 | "Back" |
| 2 | "Traffic Emissions" |
| 3 | "Road Dust" |
| 4 | "Construction Work" |
| 5 | "Biomass Burning" |
| 6 | "Garbage Burning" |
| 7 | "Factory Emissions" |
| 8 | "Smoking/Vaping" |
| 9 | "Cooking" |
| 10 | "Paint/Solvents" |
| 11 | "Other Pollution" |

Selecting a tag returns `UIAction::SaveTag` with the tag index and goes to Home.

### About Screen

| Index | Content |
|---|---|
| 0 | "Exit" (selectable) |
| 1 | "Back" (selectable) |
| — | Separator line |
| — | "AirGradient Go" (static text, not a row) |
| — | "Firmware vX.Y.Z" (static text) |
| — | "Serial XXXXXXXXXXXX" (static text) |
| — | "Open Source Hardware" (static text) |

Only indices 0 and 1 are selectable rows. The info lines are rendered as static
text below a separator.

### Confirm Screen (Clear Data)

| Index | Label | Notes |
|---|---|---|
| 0 | "Exit" | Go to Home |
| 1 | "Back" | Go to Settings |
| 2 | "Clear Data?" | Disabled / non-selectable |
| 3 | "No" | Go to Settings |
| 4 | "Yes" | Returns `UIAction::ClearData`, go to Home |

## List Scrolling

Settings, SettingsChoice, and TagList support scrolling for lists longer than
the visible window.

**Visible window**: 7 items (the list area fits 7 rows after Exit and Back).

**Page-based scrolling**: Items beyond the visible window are shown in pages of
7. Navigation wraps within a page. Moving past the last item on a page advances
to the next page. Moving before the first item on a page goes back to the
previous page (or to the Back row).

```
scroll behavior:
    if selection is at Exit or Back (index 0-1):
        move linearly between 0 and 1, or into first page item
    if selection is within page:
        move within page boundaries
    if at page boundary:
        advance/retreat to adjacent page
        highlight first/last item of new page
    if at first item of first page and moving up:
        go to Back (index 1)
```

The scroll start position tracks which page of items is currently visible.

## Snackbar System

### State

```cpp
char _snackbar_text[48];        // current message
uint32_t _snackbar_deadline_ms; // expiration timestamp
```

### Lifecycle

1. **Show**: `show_snackbar(text)` copies text, sets deadline to `now + 3000 ms`
2. **Check**: `clear_expired_snackbar(now_ms)` clears if deadline passed
3. **Deliver**: `build_values()` sets `v.snackbar_text` if snackbar is active

### Trigger Points

The orchestrator or the UI Manager calls `show_snackbar()`:

| Trigger | Text | Called by |
|---|---|---|
| Lock device | "Buttons locked" | Orchestrator |
| Unlock device | "Buttons unlocked" | Orchestrator |
| Input while locked | "Long press Menu 2s to unlock" | Orchestrator |
| Start tracking | "Tracking started" | UI Manager (via menu select) |
| Stop tracking | "Tracking stopped" | UI Manager (via menu select) |
| Save tag | "Tag 'X' saved" | UI Manager (via tag select) |
| Auto-lock | "Device auto-locked" | Orchestrator |
| Clear data | "Data cleared" | UI Manager (via confirm) |

## Chart Data Extraction

The UI Manager reads from the orchestrator-provided temporary measurement cache
(array of `Measures` structs from `StorageService`) and extracts per-metric
values for the currently selected metric.

```
populate_chart(values, cache, cache_count):
    if active_metric == None or cache_count == 0:
        values.chart_samples = nullptr
        values.chart_count = 0
        return

    // Extract the selected metric's values from each cached Measures
    for i in 0..cache_count:
        sample = extract_metric(cache[i], active_metric)
        if sample is valid:
            chart_buf[valid_count++] = sample

    // Compute min/max
    values.chart_samples = chart_buf
    values.chart_count = valid_count
    values.chart_min = min(chart_buf)
    values.chart_max = max(chart_buf)
```

The extraction function maps `Metric` enum to the corresponding nested field
in the `Measures` struct:

| Metric | Measures accessor | Type | Invalid sentinel |
|---|---|---|---|
| Pm25 | `pm_a.pm_25` | float | `MeasuresInvalid::PM` (-1.0f) |
| Co2 | `co2.co2` | int | `MeasuresInvalid::CO2` (-1) |
| Temp | `temp_hum_a.temperature` | float | `MeasuresInvalid::TEMPERATURE` (-1000.0f) |
| Humidity | `temp_hum_a.humidity` | float | `MeasuresInvalid::HUMIDITY` (-1.0f) |
| Tvoc | `tvoc_nox.tvoc_index` | int | `MeasuresInvalid::TVOC` (-1) |
| Nox | `tvoc_nox.nox_index` | int | `MeasuresInvalid::NOX` (-1) |

AGo uses channel A sensors only (`pm_a`, `temp_hum_a`). Invalid sentinel
values are skipped when building the chart sample array. Integer values
(`co2`, `tvoc_index`, `nox_index`) are cast to `float` for the chart buffer.

**Chart buffer**: The UI Manager holds a local `float _chart_buf[CACHE_MAX]`
array. The `DisplayValues::chart_samples` pointer points into this buffer.
The buffer is valid until the next `build_values()` call.

## Operating Mode Side Effects

When the user changes the operating mode via Settings, the UI Manager applies
radio side effects before returning the action:

| Mode | BLE | WiFi |
|---|---|---|
| Portable | enabled | disabled |
| Stationary | disabled | enabled |
| Offline | disabled | disabled |

These flags are communicated to the orchestrator via the `UIAction::ChangeMode`
result. The orchestrator handles the actual radio enable/disable.

## Required Settings (not yet in GoSettings)

The UI Manager needs to read and write these settings. Some are entirely
missing from `go_settings.h`, others conflict with existing fields.

### New fields needed

| Setting | Type | Default | Purpose |
|---|---|---|---|
| `use_fahrenheit` | bool | false | Temperature display unit |
| `pm_use_usaqi` | bool | false | PM display format |
| `pm_interval_seconds` | int | 10 | PM sensor polling rate |
| `other_sensor_interval_seconds` | int | 10 | Non-PM sensor polling rate |

### Existing fields that need changes

| Current field | Issue | Resolution needed |
|---|---|---|
| `gps_enabled` (bool) | UI exposes 3-state GPS mode (Off / On When Tracking / Always On) | Replace with `gps_mode` enum, or add `GpsMode` enum + migrate |
| `measurement_interval_seconds` (int) | UI exposes separate PM and other sensor intervals | Either split into two fields, or redefine this as one and add the other |
| `inactivity_timeout_seconds` (int, default 30) | UI labels this "Auto Lock" with options Off/10s/30s/60s. Default 30 is fine but "Off" (0) is also valid | Allow 0 = disabled in validation. The UI-IMPLEMENTATION.md reference uses Off/10/30/60 as the option set |

These changes should be addressed in a settings.md update before implementing
the UI Manager.

## Settings Persistence

When the user changes a setting via SettingsChoice:

1. UI Manager applies the new value to a `GoSettings &` reference held
   internally (or passed via `BuildContext`)
2. UI Manager calls `save_go_settings()` to persist to NVS
3. UI Manager returns `UIAction::SettingsChanged` (no payload)
4. Orchestrator re-reads settings it cares about (intervals, GPS mode, etc.)

This matches the design decision from `common_types.md`: `SettingsChanged`
event with no payload.

## Testability

The UI Manager has no hardware or RTOS dependencies. All logic is testable on
host.

Test strategy:
- Inject controlled `BuildContext` data, assert `DisplayValues` output
- Feed `handle_input()` sequences, assert screen transitions and `UIActionResult`
- Test menu navigation: selection wrapping, disabled item skipping, scrolling
- Test metric cycling: wrap-around, display_off disabling
- Test snackbar: show, expiry, concurrent messages (newest wins)
- Test chart extraction: valid/invalid samples, empty cache, single metric

Test cases:

- Home screen: Up/Down cycles metrics in correct order, wraps at boundaries
- Home screen: Enter opens MainMenu with index 0 selected
- MainMenu: disabled items (Add Tag when not tracking) are skipped
- MainMenu: Start/Stop Tracking toggles and returns correct UIAction
- Settings: scroll pages work correctly for 9+ items
- SettingsChoice: selecting a value returns to Settings with SettingsChanged
- TagList: selecting a tag returns SaveTag with correct index
- Confirm: "Yes" returns ClearData, "No" returns to Settings
- Snackbar: clears after 3000 ms
- Chart extraction: skips invalid sentinel values from Measures
