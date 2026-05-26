# Board v1 BMS Support Spec

> **This is a spec.** It describes how v1-only BMS support will be
> built, not what currently exists. Once shipped, the corresponding
> component / service docs become the source of truth and this spec is
> deleted.

Add v1-only BMS behaviors that come online when the board carries a
BQ27427 fuel gauge. Specifically: a new BQ27427 driver, boot-time
fuel-gauge bring-up (corruption recovery + idempotent cell-config
write) in `GoHardwareBoard::init_bms()`, `PowerService` attaches the
fuel gauge for runtime use, the existing `poll_bms()` prefers FG-derived
SOC and surfaces FG telemetry, and the orchestrator's BMS poll cadence
shortens from 60 s to 10 s on v1 only. Every behavior is gated on
`board.variant() == BoardVariant::V1`. The prototype path is
bit-identical to today.

## Problem

- v1 silicon adds a TI BQ27427 Impedance-Track fuel gauge at fixed I²C
  address `0x55`. Today's firmware has no driver. SOC on v1 would
  silently fall back to the BQ25629 voltage estimate — coarse,
  inaccurate, and inverts near the SOC extremes
- The BQ27427 has product-level bring-up needs the driver cannot
  satisfy on its own:
  - The chip stores cell-specific parameters (Design Capacity, Design
    Energy, Terminate Voltage, Sleep Current) that need to match our
    cell. Writing them every boot would wipe the chip's learned Qmax /
    Ra impedance table that takes weeks of real-world cycling to
    build. The product must compare before writing
  - A previous aborted CFGUPDATE write can leave the chip with garbage
    persistent state (e.g. Design Capacity = `0x00D0` = 208 mAh,
    FullChargeCapacity in the tens of thousands). The product must
    detect this at boot and reset the chip to factory defaults before
    applying cell config — otherwise the gauge reports nonsense
    forever
- The runtime poll path in `PowerService::poll_bms()` has no FG
  awareness today: no FG read, no SOC source preference, no `src=`
  marker in the log line. Field diagnostics on v1 would be unable to
  tell whether a reported percentage came from the FG or the BQ25629
  fallback
- The orchestrator's BMS poll cadence of 60 s leaves too long a gap
  between FG snapshots on v1 during bring-up and field debugging

## Goals

- Single firmware binary supports both boards. The v1 path activates
  only when `board.variant() == BoardVariant::V1`
- BQ27427 driver lives in `components/airgradient-bms/drivers/bq27427/`
  and follows the same code-style and error-handling conventions as
  the existing `BQ25629Bms` (constructor takes I²C bus + `Config`,
  `bool init()`, methods return `bool`, file-local `TAG`,
  `esp_log.h` for driver-layer logging, `RTOS::delay_ms` for delays)
- Product-level bring-up logic — corruption-recovery decision and
  idempotent cell-config decision — lives in `GoHardwareBoard` as a
  pure helper (`evaluate_fg_state`) that is host-testable in
  isolation. The driver remains a thin chip-primitive layer
- `PowerService` learns to consume the fuel gauge through a single
  setter (`set_fuel_gauge(BQ27427 *)`). It does not own bring-up
  logic — just runtime usage
- `poll_bms()` reads FG telemetry into the existing `PowerSnapshot`,
  prefers FG SOC, falls back to BQ25629 voltage-curve estimate on
  read failure, and tags the log line with `src=FG|BMS`
- Orchestrator picks `BMS_POLL_INTERVAL_MS = 10000` on V1 and keeps
  `60000` on prototype, resolved at construction time from
  `board.variant()`
- No host tests for the BQ27427 driver itself (bus protocol —
  HIL-tested). Host tests cover only the product-side pure logic

## Non-Goals

- This spec does **not** add SHT40, LIS2DH12, TCA9536, LP5036, or the
  BuzzerService — each becomes its own future spec consuming
  `board.variant()`
- It does **not** wire any UX — Charge Cutoff toggle, admin Charge
  Current row, Battery Learning Mode, charge-done UX. Those are later
  checkpoints that consume FG telemetry via `PowerService::poll_bms`
- It does **not** ship a `.gg` golden-image export workflow or
  bqStudio integration. The chip's learned state is preserved across
  reboots by the idempotent cell-config path; manual factory-image
  baking is out of firmware scope
- It does **not** persist FG state to NVS or RTC memory. The BQ27427
  stores its own learned Qmax / Ra in its on-chip RAM; firmware does
  not snapshot it
- It does **not** add an FG-driven safety trip. The EDV cutoff
  introduced by spec 1 (`bms_cross_board_improvements.md`) uses the
  BQ25629's `battery_voltage`, which is available on both boards and
  continues to be the trip source on v1. The FG-reported voltage is
  corroboratory only
- It does **not** migrate other drivers (SPS30, SGP41, SCD4x, …) to
  the new `ag_log.h` or any other style convention. The new BQ27427
  driver follows existing `BQ25629Bms` style as-is

## Dependencies

**Depends on (hard prerequisite):**
`board_v1_pm_polarity.md` (spec 2). Every behavior in this spec is
gated on `board.variant()`, which spec 2 introduces. Spec 3 cannot
merge before spec 2.

**Depends on (soft prerequisite):**
`bms_cross_board_improvements.md` (spec 1). Spec 1 settles the
`BmsDevice` HAL surface and the cross-board safety / power features
that apply to both boards. Spec 3 does not modify the HAL — it only
**uses** the BQ25629-side BMS through the HAL spec 1 establishes. If
spec 1 is not yet merged, this spec's PowerService changes still apply
cleanly (the FG additions are orthogonal to the spec-1 HAL additions).
Recommended merge order: spec 1 → spec 2 → spec 3.

**Will be consumed by:** later UX checkpoints (Charge Cutoff toggle,
admin Charge Current, Battery Learning Mode, charge-done UX,
e-paper power dashboard). Each reads FG telemetry through
`PowerSnapshot` fields landed by this spec.

## Design

### Driver — `BQ27427`

The driver is a thin chip-primitive layer. It owns the bus protocol,
the chip-specific access constraints (BlockData starting at register
`0x40`, UNSEAL before any DM write, the 2 Hz Standard-Command polling
ceiling), and the device-type verification on `init()`. It does **not**
know what cell the product uses, what counts as a corrupted value, or
whether `write_cell_config` should run.

Code style follows the existing `BQ25629Bms` driver verbatim:
constructor takes the I²C bus handle and a `Config`, `bool init()`
returns success, all public methods return `bool`, file-local
`static constexpr const char *TAG = "BQ27427";`, `esp_log.h` for
logging, `RTOS::delay_ms` for any delay. Header location:
`components/airgradient-bms/drivers/bq27427/bq27427.h`.

```cpp
class BQ27427 {
public:
  /// Fixed 7-bit I²C address.
  static constexpr uint8_t DEFAULT_ADDRESS = 0x55;

  /// Expected response from Control(DEVICE_TYPE) used to confirm part.
  static constexpr uint16_t DEVICE_TYPE_BQ27427 = 0x0427;

  struct Config {
    uint8_t address = DEFAULT_ADDRESS;
    uint32_t scl_speed_hz = 400000;
    int timeout_ms = 100;
  };

  /// Cell configuration block (State subclass, 0x52).
  struct CellConfig {
    uint16_t design_capacity_mah;
    uint16_t design_energy_mwh;
    uint16_t terminate_voltage_mv;
    uint16_t sleep_current_ma;
  };

  explicit BQ27427(i2c_master_bus_handle_t bus);
  BQ27427(i2c_master_bus_handle_t bus, const Config &config);
  ~BQ27427();

  BQ27427(const BQ27427 &) = delete;
  BQ27427 &operator=(const BQ27427 &) = delete;

  /// Probe, attach to the bus, verify Control(DEVICE_TYPE) == 0x0427.
  /// Idempotent.  @return true on success.
  bool init();
  bool ready() const;

  // -- Standard Command reads ----------------------------------------
  bool read_soc_percent(uint8_t &out);              // 0x1C
  bool read_voltage_mv(uint16_t &out);              // 0x04
  bool read_average_current_ma(int16_t &out);       // 0x10
  bool read_average_power_mw(int16_t &out);         // 0x18
  bool read_remaining_capacity_mah(uint16_t &out);  // 0x2A
  bool read_full_charge_capacity_mah(uint16_t &out);// 0x2E
  bool read_internal_temperature_c(float &out);     // 0x1E (0.1 K raw)
  bool read_flags(uint16_t &out);                   // 0x06

  // -- Control() subcommand ------------------------------------------
  bool control_subcommand(uint16_t subcmd, uint16_t &result);

  // -- Data Memory ---------------------------------------------------
  /// DM read; does not enter CFGUPDATE; does not perturb learned state.
  bool read_design_capacity_mah(uint16_t &out);

  /// DM read of the full 4-field CellConfig block.  Same property
  /// as read_design_capacity_mah — non-perturbing.
  bool read_cell_config(CellConfig &out);

  /// Atomic write of all four CellConfig fields in a single CFGUPDATE
  /// session.  ALWAYS enters CFGUPDATE — caller is responsible for
  /// only invoking this when the persisted values actually differ
  /// from the desired target (see GoHardwareBoard's evaluate_fg_state
  /// helper for the gating decision).
  bool write_cell_config(const CellConfig &cfg);

  /// Issue Control(RESET = 0x0041).  Full device reset; reloads all
  /// RAM data memory from chip ROM defaults.  Used to recover from
  /// corrupted persistent state.
  bool reset_to_factory_defaults();

private:
  // ... internal helpers: _read_word, _write_word, _read_block,
  // _write_block, _select_data_block, _wait_cfgupdate_flag, _unseal ...
};
```

Notes the driver enforces internally — not visible to callers:

- BlockData reads start at register `0x40` exactly. Reads starting at
  higher offsets silently return zeros; the driver hides this from
  consumers by always starting block I/O at `0x40` (TRM §4.1 / §6.4).
- UNSEAL (`Control(0x8000)` twice) runs before every DM write because
  `BlockDataChecksum (0x60)` requires UNSEALED access. Idempotent — a
  no-op when the chip is already UNSEALED (TRM §7.1.3).
- The 2 Hz Standard-Command polling ceiling (TRM §6.3.1.3) is **not**
  enforced by the driver. It's a documented contract on the caller.
  Spec consumers all poll at 10 s on V1 and 60 s on prototype, both
  well below the limit.
- `Control(DEVICE_TYPE) == 0x0427` is checked inside `init()`. On
  mismatch, `init()` returns false and `ready()` continues to return
  false.

### Product-level bring-up — `evaluate_fg_state`

A pure helper that looks at the FG's current persistent state and
decides what to do. Lives in `products/go/main/go_hardware_board.h`
(declaration) and `.cpp` (definition). Host-testable.

```cpp
// products/go/main/go_hardware_board.h — additions

struct FgRecoveryDecision {
  bool needs_factory_reset; ///< True when DC or FCC is out of range
  bool needs_config_write;  ///< True when current CellConfig differs
                            ///< from target
};

/// Decide whether the BQ27427 needs a factory reset and / or a fresh
/// cell-config write based on what it currently has in persistent
/// memory.  Pure function, host-testable.
///
/// Sanity ranges and target CellConfig are passed in so the helper
/// has no implicit dependency on file-scope constants and so tests
/// can exercise the full decision space.
FgRecoveryDecision evaluate_fg_state(
    uint16_t current_design_capacity_mah,
    uint16_t current_full_charge_capacity_mah,
    const BQ27427::CellConfig &current_cell_config,
    const BQ27427::CellConfig &target_cell_config,
    uint16_t dc_sanity_min_mah,
    uint16_t dc_sanity_max_mah,
    uint16_t fcc_sanity_max_mah);
```

Decision rules (in plain English; the implementation is a few
`if`-statements):

- `needs_factory_reset = true` when
  `current_design_capacity_mah < dc_sanity_min_mah` OR
  `current_design_capacity_mah > dc_sanity_max_mah` OR
  `current_full_charge_capacity_mah > fcc_sanity_max_mah`
- `needs_config_write = true` when any of the four target fields
  differs from the current value. (Always true when
  `needs_factory_reset` is true, because the post-reset values come
  from chip ROM defaults and will not match our cell.)

File-scope constants in `go_hardware_board.cpp` carry the actual
numbers:

```cpp
// products/go/main/go_hardware_board.cpp — file-scope constants

static constexpr BQ27427::CellConfig kAgoCellConfig = {
    .design_capacity_mah  = 2000,
    .design_energy_mwh    = 7400,
    .terminate_voltage_mv = 3000,
    .sleep_current_ma     = 50,
};

// NOTE: sanity ranges and CellConfig values are inherited from the
// partner branch, where they have been validated on hardware against
// AGo's single-cell 2000 mAh Li-ion pack.  Revisit if cell sourcing
// changes.
static constexpr uint16_t FG_DC_SANITY_MIN_MAH  = 500;
static constexpr uint16_t FG_DC_SANITY_MAX_MAH  = 8000;
static constexpr uint16_t FG_FCC_SANITY_MAX_MAH = 8500;
```

### Boot-time integration — `init_bms()`

`GoHardwareBoard::init_bms()` gains a V1 branch that constructs the
fuel gauge, reads its current state, runs `evaluate_fg_state`, and
applies whatever recovery / write the helper recommends. The
BQ25629-side init is unchanged.

Sequence on V1:

```cpp
// products/go/main/go_hardware_board.cpp — init_bms() additions
// (sketch; final code follows existing style)

if (_variant == BoardVariant::V1) {
  _fuel_gauge = new BQ27427(_i2c_bus);
  if (!_fuel_gauge->init()) {
    AG_LOGE(TAG, "BQ27427 init failed — FG offline");
    // Continue: _fuel_gauge stays non-null but ready() == false.
    // attach_fuel_gauge below will no-op on !ready().
  } else {
    uint16_t dc = 0, fcc = 0;
    BQ27427::CellConfig current{};
    const bool dc_ok  = _fuel_gauge->read_design_capacity_mah(dc);
    const bool fcc_ok = _fuel_gauge->read_full_charge_capacity_mah(fcc);
    const bool cfg_ok = _fuel_gauge->read_cell_config(current);

    const FgRecoveryDecision decision = evaluate_fg_state(
        dc_ok  ? dc  : 0,
        fcc_ok ? fcc : 0,
        cfg_ok ? current : BQ27427::CellConfig{},
        kAgoCellConfig,
        FG_DC_SANITY_MIN_MAH, FG_DC_SANITY_MAX_MAH,
        FG_FCC_SANITY_MAX_MAH);

    if (decision.needs_factory_reset) {
      AG_LOGW(TAG, "BQ27427 corrupted state (dc=%u fcc=%u) — resetting",
              dc, fcc);
      _fuel_gauge->reset_to_factory_defaults();
    }
    if (decision.needs_config_write) {
      AG_LOGI(TAG, "BQ27427 applying cell config");
      _fuel_gauge->write_cell_config(kAgoCellConfig);
    } else {
      AG_LOGI(TAG, "BQ27427 cell config already correct — preserved");
    }

    // One-shot diagnostic snapshot.
    uint8_t soc = 0; uint16_t mv = 0; int16_t ma = 0; float tc = 0.0f;
    _fuel_gauge->read_soc_percent(soc);
    _fuel_gauge->read_voltage_mv(mv);
    _fuel_gauge->read_average_current_ma(ma);
    _fuel_gauge->read_internal_temperature_c(tc);
    AG_LOGI(TAG, "BQ27427 boot: soc=%u%% v=%umV i=%dmA t=%.1fC",
            soc, mv, ma, tc);
  }
}
```

The "cell config already correct — preserved" log line is the field
signal that the idempotent path saved the chip's learned Qmax / Ra
across this boot.

### `PowerService` integration

`PowerService` gains a single setter, a non-owning pointer field, and
FG awareness inside `poll_bms()`. No bring-up logic — that's done by
the time `set_fuel_gauge()` is called.

```cpp
// products/go/main/go_power.h — additions

class PowerService {
public:
  /// Attach an already-initialised fuel gauge for runtime use.
  /// Non-owning: the fuel gauge must outlive PowerService.
  /// Pass nullptr (or skip the call entirely) on prototype boards.
  void set_fuel_gauge(BQ27427 *fg);

  // ... existing API ...

private:
  BQ27427 *_fg = nullptr;
  // ... existing members ...
};
```

`PowerSnapshot` gains FG telemetry fields. Existing fields keep their
meaning — `battery_percentage` is still the canonical SOC value; the
FG-specific fields carry additional detail for display and logging.

```cpp
// products/go/main/go_power.h — PowerSnapshot additions

enum class BatteryPercentSource : uint8_t {
  Unknown,
  FuelGauge,      ///< Read from BQ27427 (V1 with FG attached, read OK)
  BatteryCharger, ///< Voltage-curve estimate from BQ25629 (fallback)
};

const char *bms_battery_percent_source_str(BatteryPercentSource s);

struct PowerSnapshot {
  // ... existing fields ...

  BatteryPercentSource battery_percent_source = BatteryPercentSource::Unknown;

  // FG snapshot (populated only when an FG is attached and the read
  // succeeded).  Invalid sentinels otherwise.
  uint8_t  fg_soc_percent           = 255;       // 255 == invalid
  uint16_t fg_voltage_mv            = BmsInvalid::VOLTAGE_MV;
  int16_t  fg_current_ma            = BmsInvalid::CURRENT_MA;
  int16_t  fg_power_mw              = BmsInvalid::CURRENT_MA;
  uint16_t fg_remaining_capacity_mah = BmsInvalid::VOLTAGE_MV;
  uint16_t fg_full_charge_capacity_mah = BmsInvalid::VOLTAGE_MV;
  float    fg_internal_temperature_c = BmsInvalid::PERCENT;
  uint16_t fg_flags                 = 0;
};
```

`poll_bms()` reads the FG snapshot before deciding SOC source:

```cpp
// products/go/main/go_power.cpp — poll_bms() additions (sketch)

PowerSnapshot PowerService::poll_bms() {
  PowerSnapshot status{};

  // ... existing BMS telemetry / status reads ...

  // FG snapshot (V1 path).  Reads are independent; partial failures
  // leave individual fields at their invalid sentinels.
  bool fg_soc_ok = false;
  uint8_t fg_soc = 255;
  if (_fg != nullptr && _fg->ready()) {
    fg_soc_ok = _fg->read_soc_percent(fg_soc);
    if (fg_soc_ok) {
      status.fg_soc_percent = fg_soc;
    }
    _fg->read_voltage_mv(status.fg_voltage_mv);
    _fg->read_average_current_ma(status.fg_current_ma);
    _fg->read_average_power_mw(status.fg_power_mw);
    _fg->read_remaining_capacity_mah(status.fg_remaining_capacity_mah);
    _fg->read_full_charge_capacity_mah(status.fg_full_charge_capacity_mah);
    _fg->read_internal_temperature_c(status.fg_internal_temperature_c);
    _fg->read_flags(status.fg_flags);
  }

  // SOC source preference: FG first; BQ25629 voltage estimate fallback.
  if (fg_soc_ok) {
    status.battery_percentage    = static_cast<float>(fg_soc);
    status.battery_percent_source = BatteryPercentSource::FuelGauge;
  } else {
    float pct = -1.0f;
    if (_bms.get_battery_percentage(&pct)) {
      status.battery_percentage    = pct;
      status.battery_percent_source = BatteryPercentSource::BatteryCharger;
    }
  }
  status.critical = (status.battery_percentage >= 0.0f &&
                     status.battery_percentage < BATTERY_CRITICAL_PERCENT);

  // ... existing log line, with src= marker appended ...
  AG_LOGI(TAG,
          "poll_bms: perc=%.1f%% src=%s vbat=%.1fV ...",
          status.battery_percentage,
          bms_battery_percent_source_str(status.battery_percent_source),
          status.battery_voltage /* ... */);

  return status;
}
```

Wiring in `GoHardwareBoard::power()`:

```cpp
PowerService &GoHardwareBoard::power() {
  if (!_power) {
    _power = new PowerService(*_bms_driver, gpio::native::hal, { /* ... */ });
    _power->init_ext_watchdog();
    if (_fuel_gauge != nullptr && _fuel_gauge->ready()) {
      _power->set_fuel_gauge(_fuel_gauge);
    }
  }
  return *_power;
}
```

### Orchestrator cadence — variant-gated

`BMS_POLL_INTERVAL_MS` resolves at construction time from
`board.variant()`. No runtime switching, no extra branching in the
poll loop:

```cpp
// products/go/main/go_orchestrator.h — replace the static constexpr
// with an instance member set at construction.

class Orchestrator {
public:
  // ... existing ...

private:
  const uint32_t _bms_poll_interval_ms; // V1 -> 10000, Prototype -> 60000
  // ... existing ...
};
```

```cpp
// products/go/main/go_orchestrator.cpp — constructor body excerpt
Orchestrator::Orchestrator(/* ... existing args, including a GoBoard& ... */)
  : _bms_poll_interval_ms(board.variant() == BoardVariant::V1
                              ? 10000u
                              : 60000u),
    /* ... */ {}
```

Every existing `BMS_POLL_INTERVAL_MS` use site inside the Orchestrator
becomes `_bms_poll_interval_ms`. The `BMS_STATUS_POLL_INTERVAL_MS`
constant (1 Hz / 5 s, spec 1's responsibility) is **not** changed by
this spec.

### What this spec does not change

- `BmsDevice` HAL surface — unchanged. Spec 1 owns those additions
- `BMS_STATUS_POLL_INTERVAL_MS` — unchanged. Spec 1 owns it
- EDV / OT ship-mode trips — unchanged. They use BQ25629 voltage /
  battery_temperature_c, which are present on both boards. Spec 1
  owns them
- Charging defaults (`BQ25629_Config`) — unchanged
- Sleep / wake / `set_pm_power` polarity — owned by specs 1 and 2

## Implementation Plan

Each step is a focused commit.

1. **New `BQ27427` driver skeleton** — `bq27427.h` and `bq27427.cpp`
   with constructor, destructor, `init()`, `ready()` declarations and
   stub bodies. Add the new sources to
   `components/airgradient-bms/CMakeLists.txt`. Builds clean, no
   behavior change.
2. **Standard Command reads** — implement `_read_word` plus the eight
   Standard Command read methods (`read_soc_percent`,
   `read_voltage_mv`, `read_average_current_ma`,
   `read_average_power_mw`, `read_remaining_capacity_mah`,
   `read_full_charge_capacity_mah`, `read_internal_temperature_c`,
   `read_flags`). No caller yet.
3. **`Control()` subcommand + DEVICE_TYPE verify in `init()`** —
   implement `control_subcommand` and have `init()` confirm
   `Control(DEVICE_TYPE) == 0x0427`. On mismatch, `init()` returns
   false and `ready()` stays false.
4. **Data Memory plumbing** — UNSEAL helper, BlockData read/write
   starting at `0x40`, `_select_data_block`, `_wait_cfgupdate_flag`.
   Pure internal helpers.
5. **DM read APIs** — `read_design_capacity_mah` and
   `read_cell_config`. Non-perturbing reads (no CFGUPDATE entry).
6. **DM write API + factory reset** — `write_cell_config` (full
   CFGUPDATE session, always writes), `reset_to_factory_defaults`
   (`Control(0x0041)` and recovery confirmation). Self-contained — no
   integration yet.
7. **`evaluate_fg_state` helper + product constants** — declare
   `FgRecoveryDecision` and `evaluate_fg_state` in
   `go_hardware_board.h`; define in `go_hardware_board.cpp`; add
   `kAgoCellConfig`, `FG_DC_SANITY_MIN_MAH`,
   `FG_DC_SANITY_MAX_MAH`, `FG_FCC_SANITY_MAX_MAH` as file-scope
   constants. NOTE comment cites the partner branch as the source of
   the validated values.
8. **`init_bms()` V1 branch** — construct `BQ27427`, run the helper,
   apply recovery / write config, emit the boot snapshot log line.
   `_fuel_gauge` field added to `GoHardwareBoard`. Gated on
   `_variant == V1`.
9. **`PowerService::set_fuel_gauge` + `_fg` field** — pointer
   setter, non-owning. No `poll_bms` change yet.
10. **`PowerSnapshot` FG fields + `BatteryPercentSource` enum** —
    extend the struct, add the enum and its `_str()` helper, update
    `BmsInvalid` if any new sentinel is needed. No `poll_bms` change
    yet.
11. **`poll_bms` FG read + SOC preference + `src=` log marker** —
    read FG into the snapshot when `_fg != nullptr && _fg->ready()`,
    choose SOC source, append the marker to the log line.
12. **Wire FG attach in `GoHardwareBoard::power()`** — call
    `_power->set_fuel_gauge(_fuel_gauge)` when the FG is present and
    ready.
13. **Orchestrator cadence** — replace
    `static constexpr BMS_POLL_INTERVAL_MS` with an instance member
    set from `board.variant()`. Update every use site inside the
    orchestrator. Existing `BMS_STATUS_POLL_INTERVAL_MS` (spec 1)
    untouched.
14. **Host tests — `products/go/tests/go_hardware_board.tests.cpp`
    (new file)** — `evaluate_fg_state` truth table; add the test
    target to `products/go/tests/CMakeLists.txt`.
15. **Host tests — extend
    `products/go/tests/go_power.tests.cpp`** — SOC source switching
    in `poll_bms`; `set_fuel_gauge` null guard.
16. **Host tests — extend
    `products/go/tests/go_orchestrator.tests.cpp`** — cadence variant
    gating (V1 vs Prototype). Existing tests should still pass with
    the default Prototype variant.
17. **Documentation** — `components/airgradient-bms/README.md` gains
    the BQ27427 driver section (public methods, invariants, 2 Hz
    polling note); `products/go/main/go_power.{h,cpp}` doc comments
    cover the FG fields and SOC source marker; `products/go/README.md`
    "Hardware Notes" notes the FG-attached behavior on v1.

Step ordering keeps the firmware buildable at every commit. Steps 1–6
are additive driver work — invisible to the running system. Step 7
adds product-level helper logic with no caller. Step 8 lights up the
v1 boot path. Steps 9–12 wire runtime FG consumption. Step 13 is the
cadence change. Steps 14–16 are tests. Step 17 is docs.

## Testing Strategy

### Host tests (must pass before merge)

- `go_hardware_board.tests.cpp::evaluate_fg_state` — new file.
  Truth table:
  - DC at lower bound (== `FG_DC_SANITY_MIN_MAH`): not corrupted
  - DC just below lower bound: corrupted, `needs_factory_reset == true`
  - DC at upper bound (== `FG_DC_SANITY_MAX_MAH`): not corrupted
  - DC just above upper bound: corrupted
  - FCC at upper bound: not corrupted
  - FCC just above upper bound: corrupted
  - Both DC and FCC corrupted: corrupted (single trip)
  - Both DC and FCC in range, CellConfig matches target:
    `{ false, false }` — no action
  - Both in range, CellConfig differs in one field:
    `{ false, true }`
  - DC corrupted, CellConfig matches target: still both `true`
    (because post-reset values come from ROM defaults and will not
    match)
- `go_power.tests.cpp::set_fuel_gauge` — null-pointer guard;
  attaching twice is idempotent (second call just overwrites)
- `go_power.tests.cpp::poll_bms_soc_source`:
  - No FG attached: `src == BatteryCharger`, percentage from
    `_bms.get_battery_percentage()`
  - FG attached, FG SOC read OK: `src == FuelGauge`, percentage from
    FG SOC
  - FG attached, FG SOC read fails: `src == BatteryCharger`,
    percentage from BMS fallback; FG-side fields stay at sentinels
- `go_orchestrator.tests.cpp::bms_poll_cadence_variant`:
  - Orchestrator constructed with a board returning
    `BoardVariant::Prototype` → 60 s cadence
  - Orchestrator constructed with a board returning
    `BoardVariant::V1` → 10 s cadence

### Mocks / stubs

- `BQ27427` is not mocked. Host tests that exercise FG-aware code
  paths in `PowerService` use a hand-written stub of `BQ27427` that
  returns scripted values per test case. The stub follows the
  existing pattern (no Trompeloeil — `BQ27427` is a concrete class,
  not an abstract interface). The stub lives next to the existing
  `MockBmsDevice` in the test support fixtures.

### ESP-IDF build

- `idf.py -C products/go build` must succeed
- No new Kconfig symbols
- No new managed components

### Hardware-in-the-loop (user-driven)

- **Prototype board:** boot log shows no `BQ27427` lines;
  `poll_bms` log shows `src=BMS`; orchestrator cadence stays at 60 s
  (verified by log-line timestamps). Bit-identical behavior to today
- **v1 board (when silicon is available):** boot log shows
  `BQ27427 boot: soc=X% v=YmV i=ZmA t=T°C`; subsequent boots show
  `BQ27427 cell config already correct — preserved` (idempotent
  path); `poll_bms` log shows `src=FG` with FG fields populated;
  cadence verified at 10 s
- **Corruption recovery on v1:** intentionally write a bogus Design
  Capacity via bqStudio while the board is offline (e.g.
  `DC = 208 mAh`). Reboot. Boot log must show
  `BQ27427 corrupted state (dc=208 fcc=...) — resetting`, then
  `BQ27427 applying cell config`, then the boot snapshot. Subsequent
  boots show the idempotent "preserved" line again
- **FG init failure on v1:** disconnect SDA from the FG (or hold it
  low). Reboot. Boot log must show `BQ27427 init failed — FG
  offline`; `poll_bms` shows `src=BMS`; device continues to run on
  the BQ25629 estimate

## Open Questions

- **Q3.1.** `BatteryPercentSource` enum naming —
  `BatteryCharger` is a long label for what's really "BQ25629
  voltage-curve estimate". Acceptable as-is, or should it be shorter
  (`Bms`, `Charger`, `Voltage`)? Spec defaults to `BatteryCharger`
  for clarity in log lines
- **Q3.2.** Sentinels on `PowerSnapshot::fg_soc_percent` —
  `255` is used as the "invalid" marker because SOC is `0..100`.
  Acceptable, or do you want a dedicated `BmsInvalid::SOC` constant
  in `bms_types.h`? Spec defaults to local `255`
- **Q3.3.** Idempotent-CellConfig idempotency on **partial** reads —
  if `read_cell_config()` succeeds for some fields but not others
  (driver returns false), `evaluate_fg_state` is called with a
  zeroed-out current config and will recommend a write. This is
  conservative (re-writing the same values is the correct safety net
  if we genuinely don't know what's there) but it does mean a flaky
  I²C link could cause CFGUPDATE entries on every boot. Worth a
  retry path in the driver, or accept as a corner case? Spec defers
  to "accept as corner case" — flag if you'd rather the driver retry
  internally
- **Q3.4.** Should `evaluate_fg_state` accept a `_present` flag for
  each input (so it can distinguish "read failed" from "value was
  zero")? Current design uses `0` as a sentinel for "read failed",
  which is also a corrupted value, so the helper trips
  `needs_factory_reset` in that case. That's defensible — if we
  couldn't read it, resetting is the safe choice — but it's
  implicit. Spec keeps the implicit-via-zero shape for simplicity;
  if you want explicit `_present` flags, easy follow-up
- **Q3.5.** Cadence value — 10 s on V1 is inherited from the partner
  branch and motivated by FG bring-up log visibility. Is 10 s the
  right **production** cadence, or should it be tuned to 30 s / 60 s
  once the FG is trusted? Spec ships 10 s, NOTE'd as partner-
  validated; revisit when field data argues either way

## References

The "partner" branch referenced throughout this spec is the read-only
audit fork at `tmp/airgradient-firmware/` (branch `go/test/v0.3`,
upstream
[`Gingerman1996/airgradient-firmware`](https://github.com/Gingerman1996/airgradient-firmware)).
Paths below point at the partner's implementation. Where this spec
diverges from the partner (driver/product split, naming, code
location), the divergence is intentional — partner code is reference,
not template.

| Topic | Partner file | Notes |
|---|---|---|
| `BQ27427` driver | `tmp/airgradient-firmware/components/airgradient-bms/drivers/bq27427/bq27427.{h,cpp}` | We re-implement following our `BQ25629Bms` code style. Public surface differs: we drop the standalone `set_design_capacity_mah` (subsumed by `write_cell_config`), expose `read_cell_config`, and keep `write_cell_config` non-idempotent (the product decides whether to call it) |
| Idempotent CellConfig — partner version | `bq27427.cpp::configure_cell` | Partner does the diff inside the driver. We move it out: the driver always writes; `evaluate_fg_state` (product) decides whether to call |
| Corruption recovery — partner version | `tmp/airgradient-firmware/products/go/main/go_hardware_board.cpp::init_bms` (the `dc_bad` / `fcc_bad` block) | Same structure; we extract the decision into `evaluate_fg_state` so it's host-testable |
| Cell config values `{2000, 7400, 3000, 50}` | `tmp/airgradient-firmware/products/go/main/go_hardware_board.cpp` (CellConfig literal in `init_bms`) | Adopt verbatim. NOTE: validated on partner hardware |
| Sanity ranges `500..8000` / `8500` | `tmp/airgradient-firmware/products/go/main/go_hardware_board.cpp` (the same block) | Adopt verbatim. NOTE: validated on partner hardware |
| FG snapshot in `poll_bms` + SOC source marker | `tmp/airgradient-firmware/products/go/main/go_power.cpp::poll_bms` | Adopt structure |
| `BMS_POLL_INTERVAL_MS = 10000` | `tmp/airgradient-firmware/products/go/main/go_orchestrator.h` line 151 | Adopt for V1; keep 60 000 on Prototype |
| BQ27427 `Flags()` CHG bit decode (bit 8, not bit 4) | `tmp/airgradient-firmware/components/airgradient-bms/drivers/bq27427/bq27427.cpp` | Silicon behavior, not partner-specific. Documented in driver header; consumer helpers (if any) get the bit number right |

Behavioural-source-of-truth references (not partner-specific):

- BQ27427 datasheet `SLUSEB5B` (Dec 2022, rev Sep 2025) — pin map,
  Standard Command list
- BQ27427 TRM `SLUUCD5` (Jan 2023) — §4.1 / §6 Data Memory access,
  §6.3.1.3 polling ceiling, §7.1.3 UNSEAL sequence
- AirGradient read-only audit of the partner branch:
  `go_v0_3_changes_analysis.md` at repo root, "Item 2 — Battery
  Percentage (BQ27427 Fuel Gauge)" section
