# Display Menu Partial-Only Refresh — Implementation Spec

Prevent visible e-paper full/fast refreshes while the user navigates the AGo
main menu and dedicated menu pages. Menu interaction must stay responsive and
must never trigger the slow flashing refresh path, including when the user exits
back to Home.

## Goal

`DisplayService` owns refresh-tier selection. The orchestrator should not know
whether an update becomes `Full`, `Fast`, or `Partial`; it should only request a
display update when the UI or product state changes.

While navigating menu UI, every display update whose previous or next screen is
a menu-navigation screen uses a body-only partial refresh. Full/fast refreshes
are deferred until after navigation and must not be caused by the navigation
input itself.

Covered navigation includes:

- `Home` -> `MainMenu`
- `MainMenu` cursor movement
- `MainMenu` -> `Settings` / `About`
- all dedicated menu/list page movement
- `Settings` / `About` / `Confirm` / `SettingsChoice` / `TagList` back paths
- exit paths from menu/list screens back to `Home`
- action rows selected from the menu, such as Start/Stop Tracking, setting
  changes, clear data, calibration, and tag save

## Non-Goals

- Do not add orchestrator-selected display refresh policies.
- Do not make the orchestrator choose or hint `Full` / `Fast` / `Partial`.
- Do not change e-paper driver waveforms or SPI protocol.
- Do not remove anti-ghosting maintenance full refreshes globally.
- Do not make `Shutdown` or `PairingPasskey` partial-only; those are special
  system screens where a stronger visual transition is acceptable.
- Do not refresh the status bar during partial-only menu navigation. Temporary
  stale status icons are acceptable.

## Current Problem

`DisplayService::update()` currently chooses refresh mode from display diffs:

1. `Full` when `_diff_count >= Config::max_partial_ops`
2. `Partial` when both screens are navigable and the header is unchanged, or
   when staying on the same list screen
3. `Fast` otherwise

This can still flash during menu navigation:

- the anti-ghosting threshold can force `Full` on the next menu input
- status/header changes can force `Fast`
- `MainMenu` is not included in `Orchestrator::is_on_list_screen()`, so
  background status/data events can repaint while the menu overlay is open

The first two issues belong in `DisplayService`, because they are refresh-tier
decisions. The third issue is event-level display suppression and can remain an
orchestrator responsibility.

## Files

| File | Change |
|---|---|
| `products/go/main/go_display.h` | No public API change expected; add private stale-header state if kept as a class member |
| `products/go/main/go_display.cpp` | Detect menu-navigation transitions internally, force partial refreshes for them, rely on `_diff_count` to defer anti-ghosting full refresh, track stale physical header state, make diff counter safe for long navigation sessions |
| `products/go/main/go_orchestrator.h` | Rename/expand menu-screen predicate for background-update suppression only |
| `products/go/main/go_orchestrator.cpp` | Suppress background refreshes while `MainMenu` or dedicated menu screens are open; do not choose refresh tier |
| `products/go/tests/go_orchestrator.tests.cpp` | Add suppression tests for `MainMenu` if missing |
| `products/go/docs/display_service.md` | Document menu-navigation partial-only tier selection and deferred anti-ghosting |
| `products/go/docs/orchestrator.md` | Document background display suppression scope |

## DisplayService Design

### Menu-navigation screen predicate

Define the menu-navigation predicate inside `go_display.cpp` near the existing
screen helper predicates:

```cpp
bool is_menu_navigation_screen(Screen screen) {
  return screen == Screen::MainMenu || screen == Screen::Settings ||
         screen == Screen::SettingsChoice || screen == Screen::TagList ||
         screen == Screen::Confirm || screen == Screen::About;
}
```

`Home` is intentionally not a menu-navigation screen. Exit transitions back to
Home are still covered because the previous screen was a menu-navigation screen.

`Shutdown` and `PairingPasskey` are intentionally excluded and keep the existing
normal refresh-tier behavior.

### Refresh-tier decision order

`DisplayService::update()` decides the refresh tier from previous and next
`DisplayValues` only. No orchestrator hint is required.

New high-level decision order:

1. **Partial** if either previous or next screen is a menu-navigation screen.
2. **Full** if `_diff_count >= max_partial_ops`.
3. **Fast** if the physical status/header is known stale.
4. **Partial** if existing non-menu partial rules allow it.
5. **Fast** as the existing fallback.

Example implementation shape:

```cpp
const bool menu_navigation = is_menu_navigation_screen(_prev_values.screen) ||
                             is_menu_navigation_screen(values.screen);

if (menu_navigation) {
  _pending_mode = RefreshMode::Partial;
} else if (_diff_count >= _config.max_partial_ops) {
  _pending_mode = RefreshMode::Full;
} else if (_header_dirty) {
  _pending_mode = RefreshMode::Fast;
} else if (can_partial) {
  _pending_mode = RefreshMode::Partial;
} else {
  _pending_mode = RefreshMode::Fast;
}
```

Important details:

- ignore header/status changes for menu-navigation mode selection
- do not run `Fast` for header changes during menu navigation
- do not run `Full` for anti-ghosting threshold during menu navigation
- still update `_prev_values` to the new frame, so future diffs are based on
  the latest rendered UI state
- keep drawing the status bar into the software buffer; the physical display's
  status bar remains unchanged because partial writes only update `BODY_Y..end`
- if the header changed but the selected refresh is body-only `Partial`, mark
  the physical header as dirty so a later non-menu update repairs it

### Deferred status/header repair

Partial refreshes write only the body region. If status/header fields change
while a menu-navigation update is forced to `Partial`, the rendered software
buffer and `_prev_values` advance to the new state, but the physical panel's
status bar still shows the old state.

Track this explicitly with a private flag:

```cpp
bool _header_dirty = false;
```

Set the flag whenever a body-only partial refresh is selected for a frame whose
header differs from the previous logical frame:

```cpp
if (_pending_mode == RefreshMode::Partial && header_changed) {
  _header_dirty = true;
}
```

Clear the flag only after a full-screen physical update, because only `Full` and
`Fast` rewrite the status/header region:

```cpp
case RefreshMode::Full:
  _diff_count = 0;
  _header_dirty = false;
  break;

case RefreshMode::Fast:
  increment_diff_count_saturating();
  _header_dirty = false;
  break;
```

Do not clear `_header_dirty` after `Partial`.

This means menu navigation itself still never causes `Full` or `Fast`, but the
next non-menu display update may use `Fast` to repair a stale status bar.

Example:

1. BLE disconnects while `MainMenu` is open; the orchestrator suppresses that
   background display update.
2. The user exits `MainMenu` to `Home`; the update is forced `Partial` because
   the previous screen was a menu-navigation screen.
3. The new `DisplayValues` contain `ble_connected = false`; if that differs from
   the previous logical frame, `_header_dirty` is set.
4. A later sensor-data update on `Home` is non-menu. `_header_dirty` promotes it
   to `Fast`, updating the status bar without making the menu exit itself flash.

### Deferred anti-ghosting maintenance

No extra maintenance flag is needed. `_diff_count` is already the deferred
anti-ghosting signal.

When the partial-operation threshold is reached during menu navigation,
`DisplayService` still selects `Partial` because the menu-navigation rule has
higher priority than the anti-ghosting rule. The partial update increments
`_diff_count`, so the counter remains at or above the threshold.

When a later non-menu update runs in normal decision flow, the existing
`_diff_count >= max_partial_ops` check promotes that update to `Full`.

After a full refresh completes, reset `_diff_count`:

```cpp
_diff_count = 0;
```

Do not force a full refresh immediately on menu exit. The exit update itself is
partial-only because the previous screen was a menu-navigation screen. The next
natural non-menu update, or sleep preparation, can perform maintenance.

### Differential counter safety

Long menu sessions may exceed the maintenance threshold. The counter should not
wrap. Use saturating increment for differential operations:

```cpp
if (_diff_count < UINT8_MAX) {
  _diff_count++;
}
```

The full maintenance refresh later resets it to zero.

## Orchestrator Design

The orchestrator does not choose refresh tiers. It should not pass any refresh
policy and should not care whether an update becomes `Full`, `Fast`, or
`Partial`.

Its display responsibilities remain:

1. decide whether an event should cause a display update
2. build the `DisplayValues` via `UIManager`
3. call `DisplayService::update(values)`

### Background update suppression

Suppress background-triggered display refreshes while on any menu navigation
screen, including `MainMenu`:

- sensor data
- BLE connect/disconnect/auth/config writes
- BMS charging-status changes
- snackbar expiry refresh timer, unless the snackbar is system-critical

Data and status caches still update normally. BLE notifications still occur.
Only the e-paper refresh is skipped.

The existing `is_on_list_screen()` predicate should be renamed or replaced with
a clearer predicate for display suppression, for example:

```cpp
bool is_on_menu_navigation_screen() const;
```

It should include:

- `Screen::MainMenu`
- `Screen::Settings`
- `Screen::SettingsChoice`
- `Screen::TagList`
- `Screen::About`
- `Screen::Confirm`

This predicate is only for deciding whether to suppress background updates. It
does not choose the refresh tier.

## UIManager Role

No UIManager API change is required for the refresh-tier decision. The UIManager
already exposes current screen state through `DisplayValues::screen`, and
`DisplayService` can infer menu-navigation transitions from previous and next
screen values.

If future UI behavior needs more semantic detail than `Screen`, prefer adding a
field to `DisplayValues` that describes render intent/state. Do not make the
orchestrator pass refresh-tier policy.

## Refresh Behavior Matrix

| Scenario | Owner | Expected Refresh |
|---|---|---|
| Home metric browse | DisplayService existing rules | Existing behavior |
| Home -> MainMenu | DisplayService menu-transition rule | Partial |
| MainMenu cursor movement | DisplayService menu-transition rule | Partial |
| MainMenu -> Settings/About | DisplayService menu-transition rule | Partial |
| Settings/Choice/About/Confirm movement | DisplayService menu-transition rule | Partial |
| Back to MainMenu | DisplayService menu-transition rule | Partial |
| Exit menu/list -> Home | DisplayService menu-transition rule | Partial |
| Start/Stop Tracking from menu | DisplayService menu-transition rule | Partial |
| Setting change from menu | DisplayService menu-transition rule | Partial |
| BLE PairingPasskey | DisplayService existing rules | Existing Fast/Auto behavior |
| Shutdown | DisplayService existing rules | Existing Fast/Auto behavior |
| Background sensor/BLE/BMS while menu open | Orchestrator suppression | No display update |
| Deferred status repair after menu on later non-menu update | DisplayService header-dirty rule | Fast allowed |
| Deferred anti-ghosting after menu on later non-menu update | DisplayService `_diff_count` rule | Full allowed |

## Tests

### DisplayService behavior

The core behavior belongs to `DisplayService`. If the concrete service remains
excluded from host builds under `TEST_HOST`, use one of these approaches:

1. extract refresh-tier selection into a host-testable helper, or
2. add a narrow test hook that exposes the selected `RefreshMode` without
   touching hardware, or
3. validate refresh-tier behavior manually/on-device and cover surrounding
   orchestration with host tests.

Preferred direction: extract the pure mode-selection logic into a small helper
that accepts previous values, next values, current diff count, and max diff
count. This keeps hardware code untouched and makes the policy directly
testable.

Required refresh-tier cases:

1. `Home` -> `MainMenu` selects `Partial`.
2. `MainMenu` -> `Home` selects `Partial`, even when `_diff_count` is at the
   anti-ghosting threshold.
3. `Settings` -> `SettingsChoice` selects `Partial`, even when header fields
   changed.
4. `About` -> `MainMenu` selects `Partial`.
5. Menu-navigation threshold crossing selects `Partial`, not `Full`.
6. Later non-menu update with `_diff_count >= max_partial_ops` selects `Full`.
7. Full refresh resets `_diff_count`.
8. Partial refresh with header/status change sets `_header_dirty`.
9. Later non-menu update with `_header_dirty` selects `Fast` when full
   anti-ghosting is not due.
10. `Fast` and `Full` clear `_header_dirty`; `Partial` does not.
11. `Shutdown` and `PairingPasskey` transitions are not forced partial by the
   menu-navigation rule.

### Orchestrator behavior

Required suppression cases:

1. Sensor data while on `MainMenu` updates caches but does not submit a display
   update.
2. BLE connect/disconnect/auth/config updates while on `MainMenu` do not submit
   a display update.
3. BMS status changes while on `MainMenu` do not submit a display update.
4. Existing suppression for dedicated list pages remains intact.

No orchestrator test should assert a specific refresh tier. That is a
`DisplayService` responsibility.

## Risks and Tradeoffs

- **Stale status bar during menu navigation:** acceptable while the menu is open
  and during the partial-only exit update. `DisplayService` tracks this with
  `_header_dirty` and repairs it on a later non-menu full-screen update.
- **Ghosting during very long menu sessions:** acceptable short-term. The
  maintenance refresh is deferred, not removed.
- **Partial from a full-screen list back to Home:** only body updates, so the
  status bar may remain stale. This is intentional to satisfy the no full/fast
  refresh requirement for exit navigation.
- **Deferred full soon after exit could feel navigation-related:** avoid forcing
  maintenance immediately on exit. Let the next natural non-menu update handle
  it, or do it during sleep preparation.

## Acceptance Criteria

- `DisplayService` alone decides refresh tier.
- The orchestrator does not pass refresh policy and does not choose
  `Full`/`Fast`/`Partial`.
- Any update where previous or next screen is a menu-navigation screen uses
  `Partial`.
- Exiting menu/list screens back to `Home` remains partial-only.
- Anti-ghosting full refresh is deferred during menu-navigation transitions, not
  disabled globally.
- Status/header changes hidden by body-only partial refreshes are tracked by
  `DisplayService` and repaired by a later non-menu full-screen update.
- Background events do not repaint while `MainMenu` or dedicated menu pages are
  open.
- Shutdown and PairingPasskey keep existing non-menu refresh behavior.
- Existing non-menu behavior remains unchanged.
