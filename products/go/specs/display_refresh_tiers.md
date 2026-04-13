# Three-Tier Display Refresh — Implementation Spec

Replace the binary full/partial refresh decision with a three-tier strategy:
full GC, fast (full-screen differential), and partial (body-only differential).
Currently any display update that doesn't qualify for body-only partial —
header change, screen transition, snackbar on a different screen — falls
straight to a full GC refresh with visible flash. This is unnecessarily
aggressive; the SSD1680 differential waveform can update the full screen
without flash.

## Background

### Current Refresh Modes

| Mode | Mechanism | When Used |
|---|---|---|
| **Full** | `driver_hw_init_full()` + `driver_set_basemap()` — writes both RAM planes (0x24 + 0x26), trigger `0xF7`. Visible flash, ~2–3 s. | Screen transitions, header changes, snackbar on new screen, periodic anti-ghost (every 20 partials) |
| **Partial** | `driver_part_begin()` + body-only write to RAM 0x24 + `driver_part_commit()` trigger `0xFF`. No flash, ~0.3–0.5 s. | Same home-like screen with unchanged header, same list screen |

### Problem

The full refresh path is the fallback for anything that fails the
`can_partial` test. This means routine UI events — navigating to Settings,
showing a snackbar, clock minute rolling over — trigger a jarring 2–3 s
black↔white flash. The SSD1680 controller already supports full-screen
differential refresh using the same `0xFF` waveform used for body-only
partials; the driver just never applies it to the full screen.

### Decision Logic Today

```cpp
const bool can_partial = (same_home_like && !header_changed) || same_list_screen;
_pending_full = !can_partial || _partial_count >= _config.max_partial_ops;
```

Binary: `can_partial` → partial (body only), else → full (GC flash).

## Proposed Refresh Tiers

| Tier | Name | Mechanism | Waveform | Duration | When |
|---|---|---|---|---|---|
| **Full** | Full GC | `driver_hw_init_full()` + `driver_set_basemap()` | GC (`0xF7`), both RAM planes | ~2–3 s, flash | Wake from deep sleep (init), periodic anti-ghosting |
| **Fast** | Full-screen differential | `driver_part_begin()` + full-frame write to RAM 0x24 + `driver_part_commit()` | Differential (`0xFF`), single plane | ~0.5–1 s, no flash | Screen transitions, header changes, snackbar show/dismiss, lock/unlock, shutdown |
| **Partial** | Body-only differential | `driver_part_begin()` + body-region write to RAM 0x24 + `driver_part_commit()` | Differential (`0xFF`), single plane, body only | ~0.3–0.5 s, no flash | Body-only updates on home-like screens with unchanged header, same list screen |

### Why This Works at the Driver Level

Fast refresh requires no new SSD1680 commands. The existing building blocks
are sufficient:

- `driver_part_begin()` configures the controller for differential mode
- `driver_part_write_region(0, 0, data, 250, 128)` writes full frame to
  RAM plane 0x24 (4000 bytes)
- `driver_part_commit()` triggers with `0xFF` (differential waveform)

The only difference between fast and partial is the **region written**:
- Fast: full screen (X=0..15, Y=0..249) → 4000 bytes
- Partial: body only (X=0..15, Y=18..249) → 3712 bytes

SPI overhead difference: 288 bytes → ~0.6 ms at 4 MHz. Negligible.

### SSD1680 RAM Basemap Coherence

The SSD1680 differential waveform compares new data (RAM plane 0x24)
against old data (RAM plane 0x26) to determine which pixels to drive.
The controller auto-copies 0x24 → 0x26 after each partial update (trigger
`0xFF`). Evidence: the existing body-only partial path does 20 consecutive
writes to 0x24 without ever touching 0x26, and the display remains
correct across all 20 updates. This auto-copy means:

1. After a full refresh: 0x24 = 0x26 = frame A (explicit dual-plane write)
2. After a fast refresh to frame B: 0x24 was written with B, auto-copy
   updates 0x26 to B. The next differential (fast or partial) correctly
   diffs against B.
3. After a body-only partial to frame C (body changed): 0x24 body region
   was written with C, auto-copy updates the body region of 0x26.
   Header remains unchanged in both planes. Correct.

No manual basemap management is needed for the normal operation path.

## New Enum

```cpp
enum class RefreshMode : uint8_t {
    Full,     ///< Full GC waveform (flash). Resets basemap in both RAM planes.
    Fast,     ///< Full-screen differential (no flash). Writes full frame to RAM 0x24.
    Partial,  ///< Body-only differential (no flash). Writes body region to RAM 0x24.
};
```

## Decision Logic

Replaces the `_pending_full` boolean with `_pending_mode` (RefreshMode):

```
decide_refresh(prev_screen, curr_screen, header_changed, diff_count, max_diff_ops):

    // Anti-ghosting: force full after N differential operations
    if diff_count >= max_diff_ops:
        return Full

    // Body-only partial: cheapest, used when only body content changed
    same_home_like = is_home_like(prev_screen) && is_home_like(curr_screen)
    same_list      = is_list_screen(prev_screen) && prev_screen == curr_screen

    if (same_home_like && !header_changed) || same_list:
        return Partial

    // Everything else: full-screen differential (no flash)
    return Fast
```

### Full Refresh Triggers

Full GC refresh **only** occurs in two situations:

1. **Init from deep sleep** — `init()` and `init_fast_path()` always
   establish a fresh basemap via `driver_set_basemap()`. This is
   outside the worker loop decision logic.

2. **Anti-ghosting counter** — when `_diff_count >= max_diff_ops`
   (default 20) in the `update()` decision logic.

No orchestrator-level "force full" mechanism is needed. Lock, unlock,
shutdown, screen transitions, snackbar — all use fast or partial.

### Decision Matrix

| Condition | Refresh Mode |
|---|---|
| `diff_count >= max_diff_ops` | Full |
| Same home-like screen, header unchanged | Partial |
| Same list screen | Partial |
| Screen transition (any) | Fast |
| Header changed on home-like screen | Fast |
| Different list screen | Fast |
| Lock / unlock | Fast |
| Snackbar show / dismiss (on different screen type) | Fast |
| Shutdown transition | Fast |

## Anti-Ghosting Counter

The existing `_partial_count` is renamed to `_diff_count` and now counts
**all** differential operations (both fast and partial). Both waveform
types accumulate ghosting equally on the GDEY0213B74 panel.

```cpp
uint8_t _diff_count = 0;

// In worker:
if (mode == Full)    → _diff_count = 0;
if (mode == Fast)    → _diff_count++;
if (mode == Partial) → _diff_count++;
```

The `Config::max_partial_ops` field (default 20) still controls the limit.

## Files

| File | Change |
|---|---|
| `products/go/main/go_display.h` | Add `RefreshMode` enum. Replace `_pending_full` (bool) with `_pending_mode` (RefreshMode). Rename `_partial_count` → `_diff_count`. |
| `products/go/main/go_display.cpp` | Rewrite decision logic in `update()`. Rewrite worker loop to branch on three modes. Fast path writes full frame via `driver_part_write_region(0, 0, ...)`. |

No changes to `go_orchestrator.cpp`, `go_ui.cpp`, `main.cpp`, or any
other file.

## Dependencies

None. All changes use existing driver functions and SSD1680 commands.

## Worker Loop Changes

Replace the `_pending_full` branch with a three-way switch:

```cpp
void DisplayService::_worker_loop() {
    while (_running) {
        RTOS::task_notify_take(UINT32_MAX);
        if (!_running)
            break;

        esp_err_t err = driver_bus_acquire();
        if (err != ESP_OK) {
            _worker_busy = false;
            continue;
        }

        switch (_pending_mode) {
        case RefreshMode::Full:
            err = driver_hw_init_full();
            if (err == ESP_OK)
                err = driver_set_basemap(_spi_buf);
            _diff_count = 0;
            break;

        case RefreshMode::Fast:
            err = driver_part_begin();
            if (err == ESP_OK)
                err = driver_part_write_region(0, 0, _spi_buf, HEIGHT_PX, WIDTH_PX);
            if (err == ESP_OK)
                err = driver_part_commit();
            _diff_count++;
            break;

        case RefreshMode::Partial:
            err = driver_part_begin();
            if (err == ESP_OK) {
                memcpy(_region_buf, _spi_buf + BODY_Y * BUF_ROW_BYTES,
                       BODY_H * BUF_ROW_BYTES);
                err = driver_part_write_region(0, BODY_Y, _region_buf,
                                               BODY_H, SCREEN_W);
            }
            if (err == ESP_OK)
                err = driver_part_commit();
            _diff_count++;
            break;
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "worker: refresh failed: %s", esp_err_to_name(err));
        }

        driver_bus_release();
        _worker_busy = false;
    }
}
```

For the Fast path, `_spi_buf` is passed directly to
`driver_part_write_region()` — no region extraction needed.
`driver_part_write_region()` reads exactly `HEIGHT_PX * WIDTH_PX / 8`
= 4000 bytes, which fits within the 4096-byte `_spi_buf`.

## update() Decision Logic Changes

```cpp
bool DisplayService::update(const DisplayValues &values, bool wait) {
    // ... existing busy-wait logic unchanged ...

    const bool same_home_like =
        is_home_like(_prev_values.screen) && is_home_like(values.screen);
    const bool same_list_screen =
        is_list_screen(_prev_values.screen) && _prev_values.screen == values.screen;
    const bool header_changed = _is_header_changed(values, _prev_values);
    const bool can_partial =
        (same_home_like && !header_changed) || same_list_screen;

    _render_frame(values);

    if (_diff_count >= _config.max_partial_ops) {
        _pending_mode = RefreshMode::Full;
    } else if (can_partial) {
        _pending_mode = RefreshMode::Partial;
    } else {
        _pending_mode = RefreshMode::Fast;
    }

    memcpy(_spi_buf, _render_buf, sizeof(_render_buf));
    _worker_busy = true;
    _prev_values = values;

    RTOS::task_notify_give(_task_handle);
    return true;
}
```

## Edge Cases

| Scenario | Behavior |
|---|---|
| Boot (init, any path) | Full refresh via `driver_set_basemap()`. `_diff_count` = 0. |
| Home → MainMenu (menu overlay) | `is_home_like` both sides, header unchanged → Partial. Menu overlay is in body region. |
| Home → Settings (screen transition) | `is_home_like(prev)` but not `is_list_screen` match → Fast. |
| Settings → SettingsChoice (list → list) | `is_list_screen` but different screen → Fast. |
| SettingsChoice → SettingsChoice (same list, e.g. scroll) | `same_list_screen` → Partial. |
| Clock minute rolls over (Home screen) | Header changed on home-like → Fast. No flash. |
| Battery % changes (Home screen) | Header changed on home-like → Fast. No flash. |
| Lock (from any screen) | UI resets to Home. Screen transition → Fast. |
| Unlock (on Home) | Snackbar shown, screen stays Home, header unchanged → Partial. |
| Snackbar "Press button to unlock" | On Home, header unchanged → Partial. Snackbar is in body region (Y=232..249). |
| 20th differential operation | `_diff_count >= 20` → Full. Counter resets. |
| Shutdown transition | From any screen → Shutdown. Not home-like or list → Fast. |
| PairingPasskey | Orchestrator-set screen. Not home-like or list → Fast. |
| Sensor data on list screen | Orchestrator skips `update_display()`. No refresh. |
| BLE connect while on Home | Header changed (BLE status) → Fast. |

## Interaction with display_sleep_optimization.md

The `display_sleep_optimization.md` spec addresses the **timer-wake
fast-path** boot, which is a separate code path from the normal worker
loop. The two specs are complementary:

- **This spec:** Normal operation. Worker loop decides full/fast/partial
  for each `update()` call after boot.
- **Sleep optimization spec:** Timer-wake boot. `init_fast_path()`
  decides skip/partial/full for the single synchronous refresh at boot.

The sleep optimization spec currently falls back to full refresh when the
header changes on timer wake. A future follow-up could add a "fast"
tier to `init_fast_path()` using `driver_partial_from_basemap()` with
full-screen coordinates, but that is out of scope for this spec.

## What Is Not In This Spec

- No change to `init()`, `init_fast_path()`, `update_sync()`, or `clear()`
  — those always do full refresh (correct for their use cases)
- No change to the timer-wake fast-path decision logic
- No change to `DisplayValues`, `Screen`, `UIManager`, or orchestrator
- No change to the partial refresh region constants (BODY_Y, BODY_H)
- No custom LUT loading — uses SSD1680 built-in OTP waveforms throughout
- No orchestrator-level force-full mechanism (not needed)

## Testability

The decision logic is a pure function of (prev_screen, curr_screen,
header_changed, diff_count, max_diff_ops). It can be extracted for
host testing:

```cpp
RefreshMode decide_refresh_mode(Screen prev_screen, Screen curr_screen,
                                bool header_changed, uint8_t diff_count,
                                uint8_t max_diff_ops);
```

### Test Scenarios

| Scenario | Expected |
|---|---|
| Same Home, no header change, count < max | Partial |
| Same Home, header changed, count < max | Fast |
| Home → Settings, count < max | Fast |
| Settings → Settings (same list), count < max | Partial |
| Settings → SettingsChoice, count < max | Fast |
| Any transition, count >= max | Full |
| Home → Home, no header change, count = max − 1 | Partial |
| Home → Home, no header change, count = max | Full |
