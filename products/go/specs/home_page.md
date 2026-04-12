# AGo — Home Page UI Specification

Applies to: `products/go` — Home screen rendering in
`DisplayService::_draw_home()` and status bar in
`DisplayService::_draw_status_bar()`.

This spec defines the visual layout, fonts, and coordinates for the home
page. It does NOT change menu overlays, settings screens, snackbar, or
display-off behavior — those are preserved as-is.

Reference implementation: SDL2 simulator at `/home/bles/Personal/sdl/go_ui_sim/go_ui_sim.c`.
Get u8g2 fonts at `/home/bles/Personal/sdl/u8g2/tools/font/build/single_font_files/`

---

## 1. Display

128×250 pixel monochrome e-paper (SSD1680), rendered via u8g2 software
framebuffer. Inverted polarity: `memset 0xFF` = white background.
Draw color `0` = black pixels, `1` = white pixels, `2` = XOR.

Full 128px width is used for all dividers, selection rectangles, and
hero section fills. Transparent font mode (`u8g2_SetFontMode(1)`) and
transparent bitmap mode (`u8g2_SetBitmapMode(1)`) are active.

---

## 2. Layout overview

The home page is a single full-screen view divided into vertical zones:

```
y=0..16     Status bar (header)
y=17        Header divider (full-width horizontal line)
y=18..89    PM2.5 section (primary reading)
y=90..161   CO2 section (primary reading)
y=162       Divider (full-width horizontal line)
y=163..191  Row 1: Temp / Humidity (always visible)
y=192       Divider (full-width horizontal line)
y=193..221  Row 2: TVOC / NOx  OR  Min / Max (when chart active)
y=222       Divider (full-width horizontal line)
y=223..249  Row 3: Pressure / Altitude  OR  Trend chart (when chart active)
```

All horizontal dividers span x=0..127. A vertical divider at x=64
splits the bottom grid into left and right columns.

---

## 3. Status bar (y=0..16)

Icon-based status indicators. Icons are divided into three groups:

- **Left-pinned:** Lock/Unlock — always drawn at the far left.
- **Dynamic flow:** WiFi, Link/Unlink, GPS — drawn left-to-right
  starting after the lock area, skipping any that are hidden. Each
  visible icon advances the cursor by its width plus a gap.
- **Right-pinned:** Tracking and Battery — always drawn at the far
  right. Tracking sits immediately left of Battery when visible.

### Icon definitions

| Icon | Condition | Drawing method | Approx. width |
|------|-----------|----------------|---------------|
| Lock | `locked == true` | `u8g2_font_open_iconic_all_1x_t` glyph `0xCA` | ~10 px |
| Unlock | `locked == false` | `u8g2_font_open_iconic_thing_1x_t` glyph `0x44` | ~10 px |
| WiFi | `wifi_enabled == true` | `u8g2_font_siji_t_6x10` glyph `0xE21A` | ~10 px |
| Link | `ble_enabled && ble_connected` | Two 5×5 frames + connecting bar (see below) | ~12 px |
| Unlink | `ble_enabled && !ble_connected` | Two 5×5 frames + broken lines (see below) | ~12 px |
| GPS | `gps_fix == true` | `u8g2_font_siji_t_6x10` glyph `0xE0A1` | ~10 px |
| Tracking | `tracking_active == true` | Filled ellipse, radii 2×2 | ~5 px |
| Battery | Always (when data available) | Multi-level Siji glyph via existing `battery_glyph()` logic | ~10 px |

### Layout algorithm

```
ICON_GAP     = 3          // pixels between adjacent icons
LOCK_X       = 2          // left-pinned start
BATTERY_X    = 116        // right-pinned start
TRACKING_X   = 109        // right-pinned, left of battery
BASELINE_Y   = 12         // common baseline for glyph icons
ELLIPSE_CY   = 8          // vertical center for tracking dot

cursor = LOCK_X

1. Draw Lock or Unlock at cursor.  cursor += icon_width + ICON_GAP.
2. For each of [WiFi, Link/Unlink, GPS] in order:
     if condition is true:
       draw icon at cursor
       cursor += icon_width + ICON_GAP
3. If tracking_active: draw Tracking dot at TRACKING_X (fixed).
4. Draw Battery at BATTERY_X (fixed).
```

Lock and Unlock are mutually exclusive — exactly one is always drawn.
Link and Unlink are mutually exclusive — hidden entirely when
`ble_enabled == false`.

Header divider: horizontal line at y=17, x=0..127.

### Link icon detail

The link/unlink icon is drawn with primitives relative to its cursor
position. Let `x` be the cursor value when this icon is drawn, and
`cy = 5` (vertical offset within the status bar):

**Connected (Link):**
```
DrawFrame(x, cy, 5, 5)       // left box
DrawFrame(x+6, cy, 5, 5)     // right box
DrawLine(x+3, cy+2, x+7, cy+2)  // connecting bar
```

**Disconnected (Unlink):**
```
DrawFrame(x, cy, 5, 5)       // left box
DrawFrame(x+6, cy, 5, 5)     // right box
DrawLine(x+4, cy+4, x+3, cy+4)  // broken fragments
DrawLine(x+6, cy+4, x+7, cy+4)
DrawLine(x+4, cy-2, x+3, cy-3)
DrawLine(x+6, cy-2, x+7, cy-3)
DrawLine(x+4, cy+6, x+3, cy+7)
DrawLine(x+6, cy+6, x+7, cy+7)
```

---

## 4. PM2.5 section (y=18..89)

Full-width section showing the primary PM2.5 reading.

**Label line** (baseline y=44, dual-font):
- Metric name: "PM2.5" in `u8g2_font_logisoso16_tr`
- Unit: "(ug/m3)" in `u8g2_font_helvR12_tr`
- Both drawn on the same baseline, positioned so the combined group is
  approximately centered horizontally.
- When `pm_use_usaqi` is true: name remains "PM2.5", unit becomes
  "(USAQI)".

**Value** (baseline y=84):
- Formatted PM2.5 value (1 decimal place, e.g. "18.3") in
  `u8g2_font_logisoso32_tr`.
- Horizontally centered at x=64.
- When `pm_use_usaqi` is true, displayed as integer US AQI value.

---

## 5. CO2 section (y=90..161)

Same layout pattern as PM2.5.

**Label line** (baseline y=112/113, dual-font):
- Metric name: "CO2" in `u8g2_font_logisoso16_tr` at baseline y=112
- Unit: "(ppm)" in `u8g2_font_helvR12_tr` at baseline y=113
- Approximately centered horizontally as a group.

**Value** (baseline y=153):
- CO2 integer value (e.g. "450") in `u8g2_font_logisoso32_tr`.
- Horizontally centered at x=64.

---

## 6. Bottom sensor grid (y=163..249)

A 2-column × 3-row grid. Columns separated by vertical divider at x=64.
Rows separated by horizontal dividers at y=192 and y=222.

**Fonts:**
- Labels: `u8g2_font_helvR08_tr` (regular weight)
- Values: `u8g2_font_helvB08_tf` (bold weight, full charset for ° symbol)
- Temperature value uses `DrawUTF8` for the degree symbol.

### Row 1 (y=163..191) — Always visible

| Cell | Label | Label pos | Value pos | Example |
|------|-------|-----------|-----------|---------|
| Temp (left) | "Temp" | (6, 175) | (6, 187) | "-25.7 °C" / "-13.3 F" |
| Humidity (right) | "Humidity" | (68, 175) | (68, 187) | "60 %" |

### Row 2 (y=193..221) — Conditional

**No metric selected:**

| Cell | Label | Label pos | Value pos | Example |
|------|-------|-----------|-----------|---------|
| TVOC (left) | "TVOC" | (6, 205) | (6, 217) | "142" |
| NOx (right) | "NOx" | (68, 205) | (68, 217) | "1.8" |

**Metric selected:** Replaced by Min (left) and Max (right) showing the
minimum and maximum values from the chart data, formatted with the
selected metric's unit suffix and decimal precision.

### Row 3 (y=223..249) — Conditional

**No metric selected:**

| Cell | Label | Label pos | Value pos | Example |
|------|-------|-----------|-----------|---------|
| Pressure (left) | "Pressure" | (6, 235) | (6, 247) | "1015 hPa" |
| Altitude (right) | "Altitude" | (68, 235) | (68, 247) | "45 m" |

**Metric selected:** Replaced by a trend chart polyline (see §8).

### Vertical divider behavior

- **Chart active:** vertical divider at x=64 extends from y=162 to y=222
  (rows 1–2 only). Row 3 is unobstructed for the chart.
- **No chart:** vertical divider extends from y=162 to y=249 (all 3 rows).

---

## 7. Metric selection

The user cycles through selectable metrics with left/right input:

```
None → PM2.5 → CO2 → Temp → Humidity → (wraps to None)
```

Only these four metrics are selectable. TVOC, NOx, Pressure, and Altitude
are display-only and are NOT in the cycle.

### Selection highlight

When a metric is selected, its section rectangle is filled black and all
text/glyphs within are drawn white (draw color `1`).

| Metric | x | y | w | h |
|--------|---|---|---|---|
| PM2.5 | 0 | 18 | 128 | 72 |
| CO2 | 0 | 90 | 128 | 72 |
| Temp | 0 | 163 | 64 | 29 |
| Humidity | 64 | 163 | 64 | 29 |

### When a metric is selected

1. The selected section is inverted (black fill, white text).
2. Grid rows 2–3 switch from TVOC/NOx/Pressure/Altitude to Min/Max +
   chart.

### When no metric is selected

No section is inverted. All 6 grid cells are visible. No chart is shown.

---

## 8. Chart rendering

When a metric is selected, a polyline trend chart replaces grid row 3
(y=223..249).

- **Data source:** `MeasuresAGo` cache (up to 16 data points).
- **Drawing area:** x=4..124, y=224..248.
- **Style:** Consecutive line segments connecting data points. No axes,
  no labels — just the polyline.
- **Normalization:** Y values are normalized within the drawing area
  based on the actual min/max of the data set.

Min/Max values displayed in row 2 cells are derived from the chart data
and formatted using the existing `format_chart_stat()` logic with the
selected metric's unit suffix and decimal precision.

---

## 9. Preserved behaviors

The following existing behaviors are unchanged by this spec:

- **Display-off mode:** Shows centered "AirGradient" logo when
  `display_off == true`.
- **USAQI mode:** PM2.5 label/value formatting based on `pm_use_usaqi`.
- **Fahrenheit mode:** Temperature conversion based on `use_fahrenheit`.
- **Snackbar:** Overlay at bottom of screen. Position and behavior
  unchanged.
- **Menu overlay:** `MainMenu` screen rendered on top of home content.
  Position updated — see §9.1. Menu item set reduced — see §9.2.
- **Value formatting:** All `format_*` functions, validation, and
  sentinel handling preserved.
- **Partial refresh:** Body region extraction and full/partial refresh
  decision logic unchanged.

### 9.1. Menu overlay position

The main menu overlay (`Screen::MainMenu`) covers only the bottom
sensor grid, keeping both hero sections (PM2.5 and CO2) visible.

```
MAIN_MENU_BG_Y = 162      // starts at the grid divider
MAIN_MENU_BG_H = 88       // extends to y=249 (bottom of display)
```

Menu rows start at y=163 with a 22px step, fitting exactly 4 rows:

```
y=163   Row 0    (rect bottom y=183)
y=185   Row 1    (rect bottom y=205)
y=207   Row 2    (rect bottom y=227)
y=229   Row 3    (rect bottom y=249)
```

### 9.2. Main menu rows

"Add Tag" is removed from the main menu to fit the reduced overlay
height. The menu now has 4 rows:

| Index | Label |
|-------|-------|
| 0 | Exit Menu |
| 1 | Start Tracking / Stop Tracking |
| 2 | Settings |
| 3 | About Device |

The tag list screen, `UIAction::SaveTag` event, and all tag-related
plumbing (`dispatch_tag_list`, `populate_tag_list_rows`, `open_tag_list`)
are preserved in the codebase for future use — only the menu entry
point is removed.

---

## 10. Required fonts

| Font | Usage | Repo status |
|------|-------|-------------|
| `u8g2_font_logisoso32_tr` | Hero section values | **Add** |
| `u8g2_font_logisoso16_tr` | Hero section labels | **Add** |
| `u8g2_font_helvR12_tr` | Hero section units | **Add** |
| `u8g2_font_helvR08_tr` | Grid cell labels | **Add** |
| `u8g2_font_helvB08_tf` | Grid cell values | Exists |
| `u8g2_font_open_iconic_all_1x_t` | Lock icon | **Add** |
| `u8g2_font_open_iconic_thing_1x_t` | Unlock icon | **Add** |
| `u8g2_font_siji_t_6x10` | WiFi, GPS, battery icons | Exists |
| `u8g2_font_6x10_tr` | Menu/list/other screens | Exists |
| `u8g2_font_10x20_tn` | Passkey screen | Exists |

New font `.c` files must be copied from the upstream u8g2 repository
into `components/u8g2/fonts/` and registered in the component's
`CMakeLists.txt`.

---

## 11. Implementation changes summary

| Area | Change |
|------|--------|
| `go_display.cpp` layout constants | All positions updated per §2–§6 |
| `go_display.cpp` `_draw_status_bar()` | Icon-based (§3), link/unlink for BLE |
| `go_display.cpp` `_draw_home()` | New fonts, dual-font labels, centered values, full 128px width |
| `go_display.cpp` `_draw_chart()` | Remove axes — polyline only |
| `go_display.cpp` grid helpers | New font assignments, updated cell positions |
| `go_display.cpp` menu constants | `MAIN_MENU_BG_Y = 162`, `MAIN_MENU_BG_H = 88`, row start y=163 (§9.1) |
| `go_display.h` `Metric` enum | Remove `Tvoc` and `Nox` from enum (or keep enum, remove from cycle) |
| `go_ui.cpp` `browse_metric()` | 4-metric cycle: None → Pm25 → Co2 → Temp → Humidity |
| `go_ui.cpp` grid cell drawing | TVOC/NOx always in row 2 (never selected) |
| `go_ui.cpp` `populate_menu_rows()` | Remove "Add Tag" row, 4 items total (§9.2) |
| `go_ui.cpp` `dispatch_menu()` | Update switch indices: Settings=2, About=3 |
| `components/u8g2/fonts/` | Add 6 new font files |
| `components/u8g2/CMakeLists.txt` | Register new font files |
| Logo removal | Remove `draw_logo()` call from `_draw_home()` |
