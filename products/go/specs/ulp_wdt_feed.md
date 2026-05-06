# ULP Watchdog Feed — Implementation Spec

Feed the external hardware watchdog (GPIO2) from the LP Core during deep
sleep, enabling Offline-mode sleep intervals beyond the watchdog timeout
window. Currently the last watchdog pulse happens just before
`enter_sleep()`, giving one timeout window of headroom. Any sleep longer
than that window triggers a watchdog reset.

The LP Core program is intentionally minimal — pulse a GPIO pin and
return. This lays the groundwork for future LP Core work (I2C sensor
measurements during sleep) without overbuilding now.

## Background

AGo has an external hardware watchdog connected to GPIO2 (`PIN_EXT_WDT`).
The watchdog requires a rising-edge pulse (HIGH for 20 ms, then LOW) at
least every 6 minutes. Three pulse points exist today:

| When | Where | Purpose |
|---|---|---|
| Boot | `init_power()` in `main.cpp` | First pulse after wake/power-on |
| Every 60 s | Orchestrator `check_timers()` | Periodic keep-alive |
| Before sleep | Orchestrator `prepare_for_sleep()` | Maximize timeout window during sleep |

During deep sleep, no code runs on the main CPU. The pre-sleep pulse buys
one watchdog timeout window. If the configured measurement interval
exceeds the watchdog timeout, the device resets.

### ESP32-C5 LP Core

The ESP32-C5 has a RISC-V LP (Low Power) Core that runs independently of
the main CPU power domain. It survives deep sleep and can wake
periodically on an LP timer. GPIO2 is an RTC-capable pin (`LP_IO_NUM_2`)
that the LP Core can drive directly.

Key LP Core properties:

- LP memory persists across deep sleep — binary only needs loading once
  per power-on cycle, but reloading is harmless and fast
- `ulp_lp_core_stop()` halts the LP Core immediately (does not wait for
  the current `main()` to finish)
- `PMU.lp_ext.pwr0.stall_rdy` (read-only hardware register, bit 1 of
  `PMU_LP_CPU_PWR0_REG`) indicates whether the LP Core is idle (`1`) or
  actively executing (`0`). This is a documented hardware status bit on
  a `volatile` peripheral struct — safe to read, no side effects. Note:
  ESP-IDF does not provide a public API wrapper for this field; the
  direct register read follows the same access pattern used by the
  existing LL functions for other fields on the same register
- GPIO pins used by LP Core must be switched to RTCIO mode via
  `rtc_gpio_init()` from the main CPU before LP Core can drive them

### Future LP Core work (out of scope)

The LP Core will eventually perform I2C sensor measurements during deep
sleep. This spec places the LP Core stop point in each boot path at a
location that is forward-compatible with that work: after SPI/display
init but before I2C bus init.

## Files

| File | Change |
|---|---|
| `products/go/main/ulp/wdt_feed.c` | **New.** LP Core program: pulse LP_IO_2 HIGH for 20 ms, return |
| `products/go/main/go_ulp.h` | **New.** Declare `ulp_wdt_start()`, `ulp_wdt_stop()` |
| `products/go/main/go_ulp.cpp` | **New.** Load LP Core binary, start/stop with `stall_rdy` polling, `rtc_gpio_init()` for mux transition |
| `products/go/main/main.cpp` | Call `ulp_wdt_stop()` in fast path and button-wake path; call `ulp_wdt_start()` before `enter_sleep()` in fast path |
| `products/go/main/go_orchestrator.cpp` | Call `ulp_wdt_start()` in `prepare_for_sleep()` after the final `reset_ext_watchdog()` |
| `products/go/main/go_orchestrator.h` | Add `#include "go_ulp.h"` |
| `products/go/main/CMakeLists.txt` | Add `ulp_embed_binary()` for LP Core program; add `go_ulp.cpp` to `SRCS` |
| `products/go/sdkconfig.defaults` | Enable `CONFIG_ULP_COPROC_ENABLED`, `CONFIG_ULP_COPROC_TYPE_LP_CORE` |
| `products/go/docs/power_management.md` | Document LP Core watchdog feeding lifecycle |

**Not touched:** `go_power.h`, `go_power.cpp`, `components/airgradient-common/common.h`,
`components/airgradient-common/common.cpp`. The existing ext watchdog init/reset
functions remain unchanged — the main CPU continues to own GPIO2 while awake.

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `ulp_lp_core.h` | ESP-IDF ULP component | `ulp_lp_core_run()`, `ulp_lp_core_stop()`, `ulp_lp_core_load_binary()` |
| `ulp_lp_core_gpio.h` | ESP-IDF ULP component (LP Core side) | `ulp_lp_core_gpio_*()` functions in the LP Core program |
| `ulp_lp_core_utils.h` | ESP-IDF ULP component (LP Core side) | `ulp_lp_core_delay_us()` |
| `soc/pmu_struct.h` | ESP-IDF SOC component | `PMU.lp_ext.pwr0.stall_rdy` register read |
| `driver/rtc_io.h` | ESP-IDF RTC IO driver | `rtc_gpio_init()` / `rtc_gpio_deinit()` for pin mux transition |

No new `idf_component.yml` changes — the ULP component is part of
ESP-IDF and enabled via Kconfig.

## Design Decisions

### LP Core state detection via hardware register, not shared memory

ESP-IDF's test suite uses a shared-counter polling pattern to detect LP
Core activity: increment a counter in LP memory each cycle, wait several
wakeup periods from the main CPU, check if the counter changed. This is
designed for test assertions across multiple cycles.

For the stop-before-continue use case, the `PMU.lp_ext.pwr0.stall_rdy`
hardware register is a better fit. It gives instantaneous idle/running
state with a single register read — no shared variables, no multi-cycle
wait. The LP Core executes for ~20 ms out of every 60 s (0.03% duty
cycle), so `stall_rdy` will almost always read `1` (idle) and the
spin-wait exits immediately.

### Stop point placement: after SPI, before I2C

The LP Core stop point in the button-wake path is placed after Phase 1
(SPI bus + display init) but before Phase 2 (`init_core_no_spi` which
includes I2C bus init). For the current WDT-only LP Core program this
does not matter — GPIO pulsing has no bus contention. But when the LP
Core eventually performs I2C sensor measurements, stopping it before
the main CPU initializes the I2C bus prevents bus contention. Placing
the stop point correctly now avoids a refactor later.

In the fast path, `ulp_wdt_stop()` is called early, before `init_core()`
(which includes I2C init). The stop is a no-op when the LP Core is not
running (fresh boot) and takes <1 ms when it is idle (the common case
after a timer wake).

### GPIO2 pin mux lifecycle

GPIO2 can be driven by either the regular GPIO matrix (main CPU) or the
RTCIO subsystem (LP Core). Only one should drive the pin at a time.

The mux transitions are:

| Transition | Call | Effect |
|---|---|---|
| Main CPU → LP Core | `rtc_gpio_init(GPIO_NUM_2)` | Switch pin to RTCIO mode |
| LP Core → Main CPU | `init_ext_watchdog()` → `hal.configure(GPIO_NUM_2, Output)` | Switch pin to regular GPIO mode |

On wake, the pin is in RTCIO mode (LP Core was driving it). The main
CPU does not touch GPIO2 until `init_ext_watchdog()` inside
`init_power()`, which naturally switches the mux back. Between
`ulp_wdt_stop()` and `init_ext_watchdog()`, the pin is in RTCIO mode
with its last output latched LOW — no floating or glitch.

Before sleep, `ulp_wdt_start()` calls `rtc_gpio_init(GPIO_NUM_2)` to
switch the mux to RTCIO mode, then starts the LP Core.

### No changes to init_power()

`init_ext_watchdog()` and `reset_ext_watchdog()` inside `init_power()`
remain unchanged. When the main CPU is awake, it owns GPIO2 via the
regular GPIO driver — the same as today. The LP Core is only active
during deep sleep.

### ulp_wdt_stop() is always safe to call

`ulp_wdt_stop()` polls `stall_rdy` then calls `ulp_lp_core_stop()`.
When the LP Core is not running (fresh boot, non-Offline modes),
`stall_rdy` reads `1` and `ulp_lp_core_stop()` is a harmless no-op
(clears already-cleared wakeup sources, requests sleep on an
already-sleeping core). This means every boot path can call
`ulp_wdt_stop()` unconditionally without mode checks.

### ulp_wdt_start() called unconditionally before enter_sleep()

Deep sleep is only reachable in Offline mode (gated by
`decide_sleep()`). There is no need for a mode check inside
`ulp_wdt_start()`. If a future mode adds deep sleep, the LP Core
watchdog feeding automatically applies.

### LP timer interval: 60 seconds

Matches the orchestrator's awake-mode watchdog interval. With a 6-minute
watchdog timeout, 60 s gives a 6x safety margin. The power cost of a
20 ms LP Core wake every 60 s is negligible compared to the deep sleep
baseline.

## LP Core Program

`products/go/main/ulp/wdt_feed.c`:

```c
#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"

#define WDT_LP_IO    LP_IO_NUM_2
#define WDT_PULSE_US 20000

int main(void) {
    ulp_lp_core_gpio_init(WDT_LP_IO);
    ulp_lp_core_gpio_output_enable(WDT_LP_IO);
    ulp_lp_core_gpio_set_level(WDT_LP_IO, 1);
    ulp_lp_core_delay_us(WDT_PULSE_US);
    ulp_lp_core_gpio_set_level(WDT_LP_IO, 0);
    return 0;
}
```

The LP Core startup code (`lp_core_startup.c` in ESP-IDF) calls
`main()`, re-arms the LP timer, and halts. On the next LP timer tick,
the LP Core wakes and runs `main()` again.

## Main CPU Interface

`products/go/main/go_ulp.h`:

```cpp
#pragma once

/// Start the LP Core watchdog feed program.
///
/// Loads the LP Core binary (if not already loaded), switches GPIO2 to
/// RTCIO mode, and starts the LP Core with a 60 s periodic timer.
/// Call after the final main-CPU watchdog pulse, just before
/// enter_sleep().
void ulp_wdt_start();

/// Stop the LP Core watchdog feed program.
///
/// Waits for the LP Core to finish its current cycle (polls stall_rdy),
/// then halts it and clears its wakeup sources.  Safe to call when the
/// LP Core is not running (no-op).
///
/// Does NOT switch GPIO2 back to regular GPIO mode — that happens
/// naturally when init_ext_watchdog() is called later in the boot path.
void ulp_wdt_stop();
```

`products/go/main/go_ulp.cpp` (pseudocode):

```cpp
#include "go_ulp.h"

#include <driver/rtc_io.h>
#include <soc/pmu_struct.h>
#include <ulp_lp_core.h>

#include "board_config.h"
#include "rtos.h"

// Symbols generated by ulp_embed_binary()
extern const uint8_t ulp_wdt_bin_start[] asm("_binary_ulp_wdt_bin_start");
extern const uint8_t ulp_wdt_bin_end[]   asm("_binary_ulp_wdt_bin_end");

static constexpr uint64_t ULP_WDT_INTERVAL_US = 60'000'000;  // 60 s

void ulp_wdt_start() {
    ulp_lp_core_load_binary(ulp_wdt_bin_start,
                            ulp_wdt_bin_end - ulp_wdt_bin_start);

    // Switch GPIO2 to RTCIO mode so LP Core can drive it
    rtc_gpio_init(PIN_EXT_WDT);

    ulp_lp_core_cfg_t cfg = {
        .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
        .lp_timer_sleep_duration_us = ULP_WDT_INTERVAL_US,
    };
    ulp_lp_core_run(&cfg);
}

void ulp_wdt_stop() {
    // Wait for LP Core to finish current cycle (if executing).
    // stall_rdy == 1 means idle/sleeping.  The LP Core runs for ~20 ms
    // per 60 s cycle, so this almost always exits immediately.
    while (!PMU.lp_ext.pwr0.stall_rdy) {
        RTOS::delay_ms(1);
    }
    ulp_lp_core_stop();
}
```

## Boot Path Integration

### Fast path (`run_fast_path`)

```text
run_fast_path(state):
    ISR setup for button detection
    ulp_wdt_stop()                          ← NEW: stop LP Core before I2C
    init_core()                             ← includes I2C init
    release_sleep_gpio_holds()
    init_sensors()
    warmup / measurement / GPS / storage
    init_power()                            ← init_ext_watchdog (GPIO2 → regular GPIO)
    display
    sleep decision:
      if Deep:
        save_state, display stop/deep_sleep, remove ISR
        ulp_wdt_start()                     ← NEW: start LP Core before sleep
        enter_sleep()                       ← never returns
      if promote:
        run_interactive()                   ← LP Core already stopped
```

### Button-wake path (`run_button_wake_path`)

```text
run_button_wake_path(state):
    Phase 1 (~10 ms):
      init_spi → init_display
      display.init(wake_values, defer_refresh=true)
      ulp_wdt_stop()                        ← NEW: stop LP Core after SPI, before I2C

    Phase 2 (~300 ms):
      init_core_no_spi                      ← includes I2C init (safe)
      init_sensors, GPS, touch, event queue
      init_power                            ← init_ext_watchdog (GPIO2 → regular GPIO)
      start producer tasks

    Phase 3 (~3 s):  storage, BLE
    Phase 4:         orchestrator.run()
```

### Orchestrator sleep entry (`prepare_for_sleep`)

```text
prepare_for_sleep(sleep_duration_ms):
    ... existing: display update, save snapshot, stop tasks ...
    ... existing: display deep_sleep, end route, backup cache ...
    ... existing: save RTC state with sensors_warm flag ...
    reset_ext_watchdog()                    ← existing: final main-CPU pulse
    ulp_wdt_start()                         ← NEW: start LP Core
    // enter_sleep() called by try_enter_sleep() after this returns
```

### Fresh boot / run_interactive

No explicit `ulp_wdt_stop()` call needed — LP Core is not running on
fresh power-on. The `ulp_wdt_stop()` calls in fast path and button-wake
path cover the two deep-sleep-wake scenarios. When `run_interactive()` is
reached via fresh boot, LP Core was never started.

When the orchestrator eventually calls `prepare_for_sleep()` →
`ulp_wdt_start()`, the LP Core starts for the first time.

## Build System

### CMakeLists.txt changes

`products/go/main/CMakeLists.txt` — add `go_ulp.cpp` to `SRCS` and add
`ulp_embed_binary()` after `idf_component_register()`:

```cmake
ulp_embed_binary(ulp_wdt "ulp/wdt_feed.c" "")
```

The `ulp_embed_binary()` macro handles compilation of the LP Core
program with the LP Core toolchain, generates the binary blob, and
creates the `_binary_ulp_wdt_bin_start` / `_binary_ulp_wdt_bin_end`
linker symbols.

### sdkconfig.defaults additions

```text
CONFIG_ULP_COPROC_ENABLED=y
CONFIG_ULP_COPROC_TYPE_LP_CORE=y
```

These enable the LP Core toolchain integration and LP memory reservation
in the ESP-IDF build system.

## Timing Analysis

### Boot path overhead

| Path | Added latency | Notes |
|---|---|---|
| Fast path | <1 ms | `ulp_wdt_stop()` exits immediately (stall_rdy=1); `ulp_wdt_start()` loads ~100 B binary + starts timer |
| Button wake | <1 ms | Same as fast path; stop is between Phase 1 and Phase 2 |
| Fresh boot | 0 ms | No `ulp_wdt_stop()` call |

### Deep sleep overhead

The LP Core wakes every 60 s, runs for ~20 ms (GPIO pulse), and sleeps.
LP Core sleep current is part of the LP power domain baseline. The 20 ms
active window adds negligible energy compared to the 60 s sleep period.

### Watchdog safety margin

| Interval | Margin vs 6-min timeout |
|---|---|
| 60 s LP timer | 6x safety margin (5 missed pulses before reset) |
| Worst-case boot (cold, 17 s) | Last LP pulse was at most 60 s ago; 5+ min remaining |

## Edge Cases

### LP Core mid-pulse when main CPU wakes

The LP Core is in its 20 ms GPIO pulse when the main CPU wakes from deep
sleep. `ulp_wdt_stop()` spins on `stall_rdy` for at most ~20 ms until
the pulse completes, then stops the LP Core. GPIO2 is left LOW (the LP
Core program drives it LOW at the end of the pulse before returning).

### Multiple sleep/wake cycles without power-off

The LP Core binary is reloaded on each `ulp_wdt_start()` call. This is
safe — LP memory persists across deep sleep, and reloading is idempotent.
The LP timer is reconfigured on each `ulp_lp_core_run()`, so the 60 s
interval restarts cleanly.

### Non-Offline modes

Deep sleep is only reachable in Offline mode. `ulp_wdt_start()` is only
called from `prepare_for_sleep()` and the fast-path sleep entry, both of
which are gated by `decide_sleep()` returning `SleepType::Deep`.
`ulp_wdt_stop()` is a no-op when the LP Core is not running. No mode
checks are needed in the ULP module.

### Fast-path promotion (button press during fast path)

The LP Core is stopped early in `run_fast_path()`. If the user presses
the button during warmup/measurement, the fast path promotes to
`run_interactive()`. The LP Core is already stopped — the orchestrator
feeds the watchdog via the normal 60 s timer. When the orchestrator
eventually sleeps, `prepare_for_sleep()` starts the LP Core.
