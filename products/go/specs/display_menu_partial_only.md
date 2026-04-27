# Display Menu Partial-Only Refresh — Implementation Spec

Force body-only partial e-paper refreshes for all menu-navigation transitions in
the AGo DisplayService. Menu interaction must stay responsive and must never
trigger the slow flashing (Full) or full-screen differential (Fast) refresh
path, including when the user exits back to Home.

## Goal

`DisplayService` owns refresh-tier selection. The orchestrator does not know or
hint whether an update becomes Full, Fast, or Partial — it only calls
`DisplayService::update(values)` when the UI or product state changes.

While navigating menu UI, every display update whose previous or next screen is
a menu-navigation screen uses a body-only partial refresh. Full/fast refreshes
are deferred until after navigation ends. The first non-menu update after
leaving menu navigation performs a full-screen refresh (Fast or Full) to clean
up any accumulated ghosting and stale header state.

## Scope

This spec covers **DisplayService changes only**. Background display-update
suppression while on menu screens is already implemented in the orchestrator via
`UIManager::is_on_menu_screen()` and `request_background_display_update()`.

## Non-Goals

- Do not add orchestrator-selected display refresh policies.
- Do not change e-paper driver waveforms or SPI protocol.
- Do not remove anti-ghosting maintenance full refreshes globally.
- Do not refresh the status bar during partial-only menu navigation. Temporary
  stale status icons are acceptable.

## Current Problem

`DisplayService::update()` decides refresh mode from display diffs:

```cpp
if (_diff_count >= _config.max_partial_ops) {       // → Full
  _pending_mode = RefreshMode::Full;
} else if (can_partial) {                           // → Partial
  _pending_mode = RefreshMode::Partial;
} else {                                            // → Fast
  _pending_mode = RefreshMode::Fast;
}
```

Where `can_partial` is true when both screens are navigable and the header is
unchanged, or when staying on the same list screen.

This still flashes during menu navigation:

- The anti-ghosting threshold (`_diff_count >= max_partial_ops`) forces Full on
  the next menu input after enough partial/fast ops accumulate.
- A status/header change (battery, BLE, GPS) between frames makes
  `can_partial` false, falling through to Fast.

Both are refresh-tier decisions that belong in DisplayService.

## Files

| File | Change |
|---|---|
| `products/go/main/go_display.h` | Add `bool _menu_exited = false` private member |
| `products/go/main/go_display.cpp` | New menu-navigation predicate, new refresh-tier decision order, `_menu_exited` tracking, saturating `_diff_count` increment |
| `products/go/docs/display_service.md` | Update "Decision Logic" and "Decision Matrix" sections to reflect new tier order |

## Design

### Menu-navigation screen predicate

Add to the anonymous-namespace helper predicates near the existing
`is_home_like`, `is_list_screen`, and `is_navigable`:

```cpp
bool is_menu_navigation_screen(Screen screen) {
  return is_navigable(screen) && screen != Screen::Home;
}
```

This covers: MainMenu, Settings, SettingsChoice, TagList, Confirm, About.

Home is intentionally excluded. Exit transitions back to Home are still covered
because the **previous** screen was a menu-navigation screen.

### Refresh-tier decision order

New decision order in `DisplayService::update()`:

1. **Partial** — if this is a menu-navigation transition (see below)
2. **Full** — if `_diff_count >= max_partial_ops` (anti-ghosting)
3. **Fast** — if `_menu_exited` (post-menu cleanup)
4. **Partial** — if existing `can_partial` rules allow it
5. **Fast** — fallback

A transition is **menu-navigation** when either the previous or next screen is
a menu-navigation screen, **unless** the next screen is Shutdown or
PairingPasskey (system screens that benefit from a clear visual transition):

```cpp
const bool entering_system_screen =
    values.screen == Screen::Shutdown || values.screen == Screen::PairingPasskey;
const bool menu_navigation =
    !entering_system_screen &&
    (is_menu_navigation_screen(_prev_values.screen) ||
     is_menu_navigation_screen(values.screen));

if (menu_navigation) {
  _pending_mode = RefreshMode::Partial;
  _menu_exited = true;
} else if (_diff_count >= _config.max_partial_ops) {
  _pending_mode = RefreshMode::Full;
} else if (_menu_exited) {
  _pending_mode = RefreshMode::Fast;
} else if (can_partial) {
  _pending_mode = RefreshMode::Partial;
} else {
  _pending_mode = RefreshMode::Fast;
}

// Clear flag after any full-screen refresh (Full or Fast rewrites everything).
if (_pending_mode != RefreshMode::Partial) {
  _menu_exited = false;
}
```

Important details:

- The existing `can_partial`, `header_changed`, `both_navigable`,
  `same_list_screen`, and `_defer_header_check` logic is preserved unchanged
  for non-menu-navigation transitions.
- The menu-navigation rule ignores header/status changes. During menu
  navigation, partial writes only update `BODY_Y..end`; the physical status bar
  stays as-is.
- The software buffer and `_prev_values` still advance to the new frame so
  future diffs are based on the latest rendered state.

### Post-menu cleanup

Menu screens (overlays, full-screen lists) look very different from Home.
Body-only partial refreshes accumulate differential artifacts, and the status
bar may become stale if header fields changed while partials skipped it.

Rather than tracking header changes specifically, `_menu_exited` provides a
single flag that triggers a full-screen cleanup on the first non-menu update
after any menu navigation session — regardless of session length or whether the
header changed.

```cpp
// go_display.h — new private member alongside _diff_count
bool _menu_exited = false;
```

The flag is set during any menu-navigation Partial and cleared when a
full-screen refresh (Full or Fast) executes. This ensures:

- **Short menu peek** (enter, exit immediately): next sensor update on Home
  triggers Fast — cleans up body ghosting and refreshes the full screen.
- **Long menu session** (>20 partials): `_diff_count` exceeds threshold, so
  next non-menu update triggers Full (tier 2 fires before tier 3). Full also
  clears `_menu_exited`.
- **Header changed during menu**: Fast rewrites the full screen including
  status bar. No separate header-dirty tracking needed.

Example flow:

1. User opens MainMenu, browses Settings, exits to Home. All transitions are
   Partial. `_menu_exited` is set.
2. Sensor data arrives on Home. Non-menu update. `_diff_count` is below
   threshold. `_menu_exited` is true → Fast. Full screen refreshes cleanly.
   `_menu_exited` clears.
3. Subsequent sensor updates on Home use normal tier selection (Partial if
   header unchanged, Fast otherwise).

### Deferred anti-ghosting

No new flag needed. `_diff_count` already serves as the deferred signal.

During menu navigation, the anti-ghosting threshold may be reached or exceeded,
but the menu-navigation rule (tier 1) overrides it. The counter keeps
incrementing. When a later non-menu update runs, the existing
`_diff_count >= max_partial_ops` check (tier 2) promotes it to Full — which is
an even stronger cleanup than the Fast from `_menu_exited`.

After Full refresh, `_diff_count` resets to 0 as it does today.

### Saturating diff counter

Prevent `uint8_t` wrap during long menu sessions. Change both `_diff_count++`
sites in the worker loop to saturating increment:

```cpp
if (_diff_count < UINT8_MAX) {
  _diff_count++;
}
```

The Full refresh branch already resets to 0 — no change needed there.

## Refresh Behavior Matrix

| Scenario | Refresh | Why |
|---|---|---|
| Home → MainMenu | Partial | next is menu-nav |
| MainMenu cursor movement | Partial | both menu-nav |
| MainMenu → Settings/About | Partial | both menu-nav |
| Settings/Choice/About/Confirm/TagList movement | Partial | both menu-nav |
| Back to MainMenu from any menu page | Partial | both menu-nav |
| Exit menu → Home | Partial | prev is menu-nav |
| Start/Stop Tracking from menu action | Partial | prev is menu-nav |
| Setting change from menu | Partial | prev is menu-nav |
| Menu nav when `_diff_count >= max_partial_ops` | Partial | menu-nav overrides anti-ghost |
| Menu nav with header change | Partial | menu-nav ignores header |
| First non-menu update after short menu session | Fast | `_menu_exited` cleanup |
| First non-menu update after long menu session | Full | `_diff_count` threshold overrides `_menu_exited` |
| MainMenu → Shutdown | Fast/Full (existing) | system screen exception |
| MainMenu → PairingPasskey | Fast/Full (existing) | system screen exception |
| Home metric browse | Existing behavior | no menu-nav screen involved |
| Background sensor/BLE/BMS while menu open | No update | orchestrator suppression (already done) |

## Risks and Tradeoffs

- **Stale status bar during menu navigation:** acceptable while the menu is open
  and during the partial-only exit Partial. Cleaned up by the Fast/Full on the
  next non-menu update.
- **Ghosting during very long menu sessions:** acceptable short-term. The
  maintenance Full refresh is deferred, not removed.
- **One Fast refresh after every menu exit:** even a quick menu peek triggers
  Fast on the next sensor update (~1–1.5 s, no flash). Acceptable cost for a
  consistently clean Home screen.
- **Partial from a full-screen list back to Home:** only body updates, so the
  status bar may remain stale until the post-menu Fast/Full. This is intentional
  to keep the exit transition itself flash-free.

## Verification

DisplayService refresh-tier logic is hardware-coupled and excluded from host
builds under `TEST_HOST`. Verify on device:

1. Home → MainMenu → navigate menu → exit to Home: no flash at any step.
2. Short menu peek (enter MainMenu, immediately exit): next sensor update on
   Home triggers Fast (clean full-screen refresh, no flash).
3. Long menu session (>20 partial ops): no flash during menu. First non-menu
   update after exit triggers Full (anti-ghosting maintenance).
4. Header change during menu navigation (e.g. BLE disconnect): no flash during
   menu. Post-menu Fast on next non-menu update rewrites full screen including
   status bar.
5. MainMenu → Shutdown: normal Fast/Full transition (system screen exception).
6. PairingPasskey while on menu screen: normal Fast/Full transition.
7. Home metric browse: existing behavior unchanged.
8. After post-menu cleanup Fast fires, subsequent Home updates return to normal
   tier selection (no lingering `_menu_exited`).

## Documentation Update

After implementation, update the following sections in
`products/go/docs/display_service.md`:

- **Decision Logic** — add menu-navigation rule as tier 1, add `_menu_exited`
  as tier 3
- **Decision Matrix** — add menu-navigation rows, post-menu cleanup row, system
  screen exception rows
- **Anti-Ghosting Counter** — note saturating increment and deferred behavior
  during menu navigation

## Acceptance Criteria

- Any update where previous or next screen is a menu-navigation screen uses
  Partial (body-only, no flash).
- Exception: transitions **to** Shutdown or PairingPasskey use existing
  non-menu behavior even if coming from a menu-navigation screen.
- First non-menu update after any menu navigation session uses Fast (or Full if
  anti-ghosting threshold reached) to clean up accumulated artifacts.
- Anti-ghosting Full refresh is deferred during menu navigation, not skipped.
- `_diff_count` uses saturating increment (no `uint8_t` wrap).
- Existing `_defer_header_check` boot-path behavior is preserved.
- Existing non-menu refresh behavior is unchanged.
- `docs/display_service.md` decision logic updated.
