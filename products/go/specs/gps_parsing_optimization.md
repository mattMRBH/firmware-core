# GPS Parsing Optimization — Implementation Spec

Eliminate GPS-induced e-paper display refresh slowdowns on AGo without
changing the GPS module baud rate. When the TAU1113 GPS module acquires
a satellite fix, the e-paper full refresh becomes visibly slower on the
single-core ESP32-C5. This spec introduces three layered optimizations:

1. **Bulk UART reads** in `AirgradientSerial` / `AirgradientUART` / `NmeaGps`
   (remove per-byte syscall overhead).
2. **Sentence-level filter** in `NmeaGps` (skip `nmea_parse()` and its
   `malloc`/parse/`free` cycle for unused sentence types).
3. **GPS task priority drop** below the display worker (guarantee the
   display refresh is never preempted by GPS work).

Changing the GPS baud rate is explicitly out of scope. An optional
Phase 2 trims the `libnmea-esp32` parser list and is documented as a
follow-up.

## Background

When the GPS module has no fix it emits a small number of NMEA sentences
(mostly GGA/RMC/GSA). On fix, the sentence volume grows 3–5× because GSV
satellite-in-view data is added — one GSV sentence per 4 satellites per
constellation, typically 10–15 extra sentences per second with a
multi-GNSS fix. On a single-core target, three compounding inefficiencies
make the extra load expensive for the display:

1. **Byte-by-byte UART reads.** `NmeaGps::read()` drains the UART one
   byte at a time via `_serial.read()`, each call going through
   `uart_read_bytes(&b, 1, 10 / portTICK_PERIOD_MS)`. At ~1000 bytes/sec
   during fix this is ~1000 syscalls per second on the GPS task.

2. **All sentences parsed, most discarded.** `libnmea-esp32` currently
   compiles 7 parsers (GGA, GLL, RMC, GSA, GSV, TXT, VTG). Each recognized
   sentence goes through `malloc()` → field parsing → `free()`, even
   though `NmeaGps` only consumes GGA, RMC, and GSA. GSV alone contributes
   the majority of the allocator pressure during fix.

3. **Task priority inversion relative to the display.** The GPS task
   runs at priority 5, above the display worker at priority 4. While the
   display worker polls `wait_busy_low()` during a full refresh, any work
   the GPS task does (including the per-byte reads and allocator traffic
   above) preempts the display worker, stretching the wall-clock time of
   the refresh and producing the visible slowdown.

Baud rate is fixed at 9600 for this iteration, so the optimization
targets *CPU time per incoming byte* and *who gets the CPU while the
display is refreshing*.

## Files

| File | Change |
|---|---|
| `components/airgradient-serial/include/airgradient_serial.h` | Declare new virtual `int read(uint8_t *buf, int len)` with a default implementation that loops over single-byte `read()` |
| `components/airgradient-serial/airgradient_serial.cpp` | Define the default bulk-read implementation |
| `components/airgradient-serial/include/airgradient_uart.h` | Declare `int read(uint8_t *buf, int len) override` |
| `components/airgradient-serial/airgradient_uart.cpp` | Implement bulk `read()` via `uart_read_bytes(port, buf, len, 0)` |
| `components/airgradient-gps/drivers/nmea_gps/nmea_gps.h` | Declare the `_is_accepted_sentence` static helper; add `READ_CHUNK_SIZE` constant |
| `components/airgradient-gps/drivers/nmea_gps/nmea_gps.cpp` | Replace byte-loop in `read()` with bulk-read path; add early sentence-ID filter at the `\r\n` boundary in `_process_byte()` |
| `components/airgradient-gps/tests/nmea_gps.tests.cpp` | Add test cases for filtered sentences and for GSV-does-not-disturb-GGA-state |
| `products/go/main/go_gps.h` | Lower default `Config::task_priority` from `5` to `3` |
| `products/go/main/main.cpp` | Update the two `GpsService` construction sites to pass `.task_priority = 3` (call sites currently hardcode `5`) |
| `products/go/docs/gps_service.md` | Update the priority row in the Configuration table to `3` |

**Not touched in this phase:** `products/go/main/board_config.h` (baud
stays 9600), `products/go/main/go_gps.cpp` (service loop unchanged),
`components/libnmea-esp32/CMakeLists.txt` (deferred to optional Phase 2).

## Dependencies

No new dependencies. All changes use the existing ESP-IDF UART API and
the existing `libnmea` API. No `idf_component.yml` changes.

## Design Decisions

### Bulk read declared on the base class with a default loop implementation

The new `int AirgradientSerial::read(uint8_t *buf, int len)` is
non-pure. The base class provides a default implementation that loops
over the single-byte `read()`. Only `AirgradientUART` overrides it with a
real bulk implementation.

Rationale:
- `AirgradientIICSerial` already has its own software RX buffer behind a
  WK2132 chip. The default loop-over-single-byte path is correct and has
  no worse performance than what it does today.
- Existing sensor drivers (PMS5003, SenseAir, SGP41) that use the
  single-byte API are entirely unaffected.
- The host-test `StubSerial` in `components/airgradient-gps/tests` needs
  no changes: it inherits the default bulk read, which dispatches back
  into its single-byte `read()` override. Existing tests compile and
  pass without modification.

### Zero timeout on the UART bulk read

`NmeaGps::read()` gates the bulk call on `_serial.available() > 0`, so
bytes are known to be ready. The semantic is "take whatever is in the
FIFO, don't block". Passing `0` ticks to `uart_read_bytes` avoids
entering the driver's blocking path and keeps the GPS task yield cadence
strictly at `TASK_YIELD_MS = 10 ms`. The current per-byte path passes
`10 / portTICK_PERIOD_MS`; when the FIFO has data the actual wait is
zero, but the kernel still enters the blocking codepath once the FIFO
drains mid-burst. The new path is strictly non-blocking.

### 512-byte read chunk on the GPS task stack

`NmeaGps::read()` reads at most `READ_CHUNK_SIZE = 512` bytes per call.
At 9600 baud the worst-case ring-buffer fill during a preempted GPS
task is ~480 bytes for a ~500 ms preemption window, which drains in a
single bulk call. The existing GPS task stack is 4096 B, so 512 B is
~12% and well within budget. Any residual bytes beyond 512 are picked
up on the next 10 ms cycle; the 1024 B UART ring buffer absorbs
everything in between.

A smaller 256 B buffer with an internal drain loop was considered but
rejected as more code for no measurable benefit.

### Sentence-level filter at the `\r\n` boundary in `_process_byte()`

The filter lives in `_process_byte()` at the moment the terminator is
detected, rather than inside `_handle_sentence()`. This keeps
`_handle_sentence()` focused on valid sentences and makes the filter
co-located with sentence-boundary detection. The accepted-sentence test
is a single static helper:

```cpp
static bool NmeaGps::_is_accepted_sentence(const char *buf, size_t len);
```

It checks `len >= 6`, then runs three `memcmp(buf + 3, "GGA"|"RMC"|"GSA", 3)`
comparisons. `memcmp` is used instead of `strncmp` because the bytes at
those offsets are guaranteed to exist after the length check and there
are no null-terminator concerns on a raw NMEA buffer.

### Talker ID assumption: 2 characters

The filter assumes a standard NMEA 0183 talker ID of exactly 2
characters (`GP`, `GN`, `GL`, `GA`, `GB`, etc.). This is correct for all
standard sentences the TAU1113 emits. Proprietary sentences of the form
`$PXXX...` have a single-character "talker" (`P`) followed by a 3-char
manufacturer ID, so comparing bytes 3..5 against the accepted IDs always
fails and the sentence is dropped. That is the correct outcome —
`NmeaGps` does not consume proprietary sentences today.

Non-standard talker IDs (e.g. a hypothetical 3-char talker) would also
be dropped. No such sentences are emitted by the TAU1113 according to
the datasheet.

### GPS task priority dropped from 5 to 3

This is the non-obvious decision. Justification:

- NMEA epochs arrive at 1 Hz. The UART RX ring buffer holds >1 second of
  data at 9600 baud (960 bytes vs 1024 B ring). Higher-priority work can
  freely preempt the GPS task without risk of RX overflow.
- `TASK_YIELD_MS = 10 ms` caps the backpressure introduced by the lower
  priority. Under normal load the GPS task never misses a 10 ms window.
- Current AGo priorities: Input=6, GPS=5, SensorProducer=5, Display=4,
  Idle=0. Dropping GPS to 3 places it strictly below the display worker
  and strictly above idle, with no conflict with any existing task.
- The display worker gains deterministic CPU during `wait_busy_low()`.
  That is the goal.
- The CPU savings from bulk reads and the sentence filter still matter
  after the priority drop: they reduce total CPU-active time (battery)
  and shorten every GPS cycle, which matters for any future higher-
  priority task that depends on GPS latency.

### Two call sites in `main.cpp` must be updated

The defaults in `GpsService::Config` are already overridden explicitly
in both `run_full_boot()` and `run_fast_path()` call sites with
`.task_priority = 5`. Changing the default in `go_gps.h` alone is not
sufficient — both call sites must also be updated to `.task_priority = 3`
or the default change is silently ignored. Both sites and the default
value are listed in the Files table.

### Filtered sentences report `read()` returns false

When `_process_byte()` drops a filtered sentence, it returns `false` to
its caller. `NmeaGps::read()` therefore does not report a "new sentence
delivered" for dropped sentences, matching the semantics "no *usable*
sentence delivered this cycle". The GPS service loop in `go_gps.cpp`
already handles this correctly: it only updates `_latest_fix` when
`read()` returns true and a valid sentence actually landed.

### Existing tests remain valid without modification

`StubSerial` in `nmea_gps.tests.cpp` implements only the single-byte
`read()` override. It inherits the default bulk `read()` from the
base class, which loops over single-byte `read()`. Behavior is
identical under test. All new tests are additive.

## API Changes

### `airgradient_serial.h`

```cpp
/**
 * Read up to @p len bytes into @p buf. Non-blocking: returns
 * immediately with whatever is available (0 if nothing). Subclasses
 * may override with a more efficient implementation. The default
 * implementation loops over single-byte read() and is correct but
 * not optimal.
 *
 * @param buf Destination buffer (must not be null)
 * @param len Maximum number of bytes to read
 * @return Number of bytes actually read (0..len)
 */
virtual int read(uint8_t *buf, int len);
```

### `airgradient_serial.cpp`

```cpp
int AirgradientSerial::read(uint8_t *buf, int len) {
  if (buf == nullptr || len <= 0) {
    return 0;
  }
  int written = 0;
  for (int i = 0; i < len; i++) {
    const int b = read();
    if (b < 0) {
      break;
    }
    buf[written++] = static_cast<uint8_t>(b);
  }
  return written;
}
```

### `airgradient_uart.h`

```cpp
/**
 * Bulk read from UART RX FIFO. Non-blocking.
 *
 * @param buf Destination buffer
 * @param len Maximum number of bytes to read
 * @return Number of bytes actually read
 */
int read(uint8_t *buf, int len) override;
```

### `airgradient_uart.cpp`

```cpp
int AirgradientUART::read(uint8_t *buf, int len) {
  if (!isInitialized || buf == nullptr || len <= 0) {
    return 0;
  }
  const int n = uart_read_bytes(_port_num, buf, len, 0);
  return (n > 0) ? n : 0;
}
```

The existing single-byte `AirgradientUART::read()` is kept as-is so
other consumers are not disturbed.

### `nmea_gps.h`

```cpp
// Bulk read chunk size used by NmeaGps::read(). Sized to drain the
// UART ring buffer in a single call even under worst-case preemption
// (~500 ms @ 9600 baud).
static constexpr size_t READ_CHUNK_SIZE = 512;

// ...
private:
  // ...

  // Return true if the sentence starting at @p buf with length @p len
  // is one of the types NmeaGps consumes (GGA, RMC, GSA). The check
  // assumes a 2-char standard NMEA talker ID at bytes 1..2 and reads
  // the sentence ID at bytes 3..5.
  static bool _is_accepted_sentence(const char *buf, size_t len);
```

### `nmea_gps.cpp` — new `read()`

```cpp
bool NmeaGps::read() {
  bool got_sentence = false;
  const int avail = _serial.available();
  if (avail <= 0) {
    return false;
  }

  uint8_t chunk[READ_CHUNK_SIZE];
  const int to_read =
      (avail < static_cast<int>(sizeof(chunk))) ? avail : static_cast<int>(sizeof(chunk));
  const int n = _serial.read(chunk, to_read);
  for (int i = 0; i < n; i++) {
    if (_process_byte(static_cast<char>(chunk[i]))) {
      got_sentence = true;
    }
  }
  return got_sentence;
}
```

### `nmea_gps.cpp` — filter in `_process_byte()`

Replace the existing terminator-detection branch:

```cpp
// Detect NMEA 0183 sentence terminator: \r\n
if (byte == '\n' && _buffer_pos >= 2 && _buffer[_buffer_pos - 2] == '\r') {
  if (_is_accepted_sentence(_buffer, _buffer_pos)) {
    _handle_sentence(_buffer, _buffer_pos);
    _buffer_pos = 0;
    return true;
  }
  // Drop filtered sentence silently — no allocator activity.
  _buffer_pos = 0;
  return false;
}
```

### `nmea_gps.cpp` — static helper

```cpp
bool NmeaGps::_is_accepted_sentence(const char *buf, size_t len) {
  // Minimum viable sentence: "$XXYYY" = 6 chars before any fields.
  if (len < 6) {
    return false;
  }
  const char *id = buf + 3; // skip "$" + 2-char talker
  return (memcmp(id, "GGA", 3) == 0) || (memcmp(id, "RMC", 3) == 0) ||
         (memcmp(id, "GSA", 3) == 0);
}
```

### `go_gps.h`

```cpp
struct Config {
  int baud_rate = 9600;
  int posting_interval_ms = 5000;
  uint16_t task_stack_size = 4096;
  uint8_t task_priority = 3; // was 5; must be below display worker (4)
};
```

### `main.cpp` — two call sites

Both the `run_full_boot()` and `run_fast_path()` sites that build a
`GpsService::Config` with `.task_priority = 5` become `.task_priority = 3`.
No other fields change.

## Edge Cases

| Scenario | Behavior |
|---|---|
| `available()` returns 0 | Early return, no syscall, no stack traffic |
| `available()` > 512 | Read 512, remainder drains next 10 ms cycle |
| Preempted ~500 ms by display | ≤480 bytes in ring, drains in a single bulk call |
| UART ring overflow | Not expected at 9600 baud; driver drops same as today |
| Bad checksum on a GGA sentence | Passes filter, `nmea_parse` rejects, state unchanged — same as today |
| GSV sentence with valid checksum | Filter drops before `nmea_parse`, zero allocator activity |
| Proprietary `$PXXX...` sentence | Filter rejects, silently discarded |
| Partial sentence at bulk boundary | Accumulated in `_buffer`, completed next cycle — same as today |
| Buffer overflow (>256 bytes without `\r\n`) | Existing overflow guard resets `_buffer_pos`, unchanged |
| Host tests using `StubSerial` | Default base-class bulk read loops over single-byte — works unchanged |
| Priority 3 vs display refresh | Display worker at 4 strictly preempts GPS; refresh is deterministic |
| Higher-priority sensor work | GPS yields naturally; no starvation because NMEA data is 1 Hz |
| Other `AirgradientSerial` users (PMS5003, SenseAir, SGP41) | Unaffected; none override bulk `read()`; all continue to use the single-byte API |

## Expected Impact

| Metric | Before | After |
|---|---|---|
| UART syscalls/sec during fix | ~1000 | ~50–100 (one per 10 ms cycle) |
| `malloc`/parse/`free` per sec during fix | ~15–18 (all sentence types) | ~3 (GGA + RMC + GSA only) |
| Display full-refresh duration during fix | visibly slower than no-fix baseline | identical to no-fix baseline |
| GPS task CPU time during fix | high, continuous | ~3–5× lower, bursty |
| Code size | baseline | small increase (bulk read paths + helper) |
| GPS task stack peak | baseline | +512 B transient (buffer is a local) |
| RTOS priority of GPS | 5 (above display) | 3 (below display, above idle) |

## Testability

### Existing tests — no changes

All existing `nmea_gps.tests.cpp` tests must continue to pass without
modification. The `StubSerial` stub has no bulk-read override, so the
base-class default dispatches back into its single-byte `read()`. This
path is identical to what the tests exercise today. If any existing
test fails after the change, the base-class default implementation is
incorrect.

### New unit tests

| Test | Scenario | Assertion |
|---|---|---|
| `nmea_gps filters GSV sentence` | Queue a valid GSV sentence, call `read()` | `read()` returns `false`; `get_data()` returns sentinel state |
| `nmea_gps filters GLL sentence` | Queue a valid GLL sentence, call `read()` | `read()` returns `false`; state unchanged |
| `nmea_gps filters VTG sentence` | Queue a valid VTG sentence, call `read()` | `read()` returns `false`; state unchanged |
| `nmea_gps filters TXT sentence` | Queue a valid TXT sentence, call `read()` | `read()` returns `false`; state unchanged |
| `nmea_gps filters proprietary $PXXX sentence` | Queue e.g. `$PMTK010,001*2E\r\n` | `read()` returns `false`; state unchanged |
| `nmea_gps ignores very short sentence` | Queue `$GP\r\n` (length < 6) | `read()` returns `false`; no crash |
| `nmea_gps GSV between GGA and RMC` | Queue GGA, GSV, RMC in sequence | After `read()`, GGA position and RMC timestamp are present; GSV is invisible to `GpsData` |
| `nmea_gps accepts GN talker ID` | Queue `$GNGGA,...` (multi-GNSS talker) | Parsed same as `$GPGGA` |

### Optional `StubSerial` enhancement (not in this spec)

A follow-up test enhancement could add an `int read(uint8_t *buf, int len)`
override to `StubSerial` to exercise the bulk path directly. This is
noted but not required for the spec to land. The default base-class
path already guarantees correctness.

### Host-test build

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

### Firmware build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/go build
```

### Hardware verification (user-run, outside agent scope)

- Boot the AGo firmware with GPS module outdoors.
- Wait for satellite fix (observe satellite count > 0, fix_type != NoFix).
- Trigger a full display refresh (e.g. via a timer wake or a value change
  that forces a full refresh).
- Observe the refresh duration — it should match the no-fix baseline
  and no longer produce a visible slowdown.
- Check `idf.py monitor` logs (user-run) for any GPS task starvation
  warnings or missed fix events.

## Verification Checklist

- [ ] `idf.py -C products/go build` succeeds
- [ ] Host test suite builds and all existing `nmea_gps` tests pass
- [ ] New filter tests pass (GSV, GLL, VTG, TXT, proprietary, short sentence)
- [ ] New multi-sentence test with GSV in the middle passes
- [ ] `AirgradientIICSerial` / `PMS5003` / `SenseAir` drivers unaffected — spot check by compiling any product that uses them
- [ ] `go_gps.h` default `task_priority` is `3`
- [ ] Both `GpsService` call sites in `main.cpp` pass `.task_priority = 3`
- [ ] `products/go/docs/gps_service.md` priority row updated to `3`
- [ ] No magic numbers added; `READ_CHUNK_SIZE` is a named constant
- [ ] `clang-format -style=file -i` applied to every modified source file
- [ ] Manual display-refresh-with-fix observation matches no-fix baseline

## Phase 2 — Optional libnmea-esp32 parser trim

After Phase 1 lands and the display slowdown is confirmed fixed, the
following optional cleanup may be considered. It is **not** part of
this spec's required changes.

### Change

In `components/libnmea-esp32/CMakeLists.txt`:

```cmake
set(parsers gpgga gprmc gpgsa)
```

This drops `gpgll`, `gpgsv`, `gpvtg`, `gptxt` from compilation, reduces
`PARSER_COUNT` from 7 to 3, shrinks the static parser lookup array,
and saves a small amount of flash.

### Rationale

Defense in depth against any future regression in the sentence filter.
With Phase 1 in place, unused sentences never reach `nmea_parse()`, so
the flash and init-time savings are the only direct benefit.

### Prerequisites

- Grep the entire repository for uses of `NMEA_GPGSV`, `NMEA_GPGLL`,
  `NMEA_GPVTG`, `NMEA_GPTXT`, `nmea_gpgsv_s`, `nmea_gpgll_s`,
  `nmea_gpvtg_s`, `nmea_gptxt_s`, and the parser module names
  (`gpgsv`, `gpgll`, `gpvtg`, `gptxt`). If any other product or
  component references them, skip this phase or gate the list per
  product.
- Verify all other products that include `libnmea-esp32` still build.

### Why it's deferred

- It touches a shared third-party-wrapper component; risk is non-zero
  relative to the local changes in Phase 1.
- Measurable benefit is small once Phase 1 is in place.
- Landing it separately keeps Phase 1's diff focused and easy to
  revert.

## What Is Not In This Spec

- GPS module baud rate change (explicitly out of scope).
- Any changes to `products/go/main/board_config.h`.
- Any changes to the `GpsService` task loop in `go_gps.cpp` (no new
  state, no new posting logic, no new event types).
- Changes to `gps_read_once()` — the synchronous fast-path one-shot
  read. It already calls `gps.begin(baud_rate)` → `gps.read()` in a
  loop, and both benefit transparently from the bulk-read and filter
  changes without any call-site edit.
- UART RX ring buffer size changes. The current 1024-byte buffer is
  sufficient at 9600 baud even under worst-case GPS task preemption.
- Per-sentence enable/disable commands at the TAU1113 module. The
  datasheet does not expose this capability; the software-side filter
  is the equivalent.
- Phase 2 libnmea parser trim is documented separately as a follow-up,
  not a required part of this spec.
