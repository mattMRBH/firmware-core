# GPS UART Optimization

Reduce GPS-related CPU load on the single-core ESP32-C5 to eliminate visible
e-paper display refresh slowdowns when the GPS module has a satellite fix.

## Problem

When the TAU1113 GPS module acquires a fix, the e-paper full refresh becomes
visibly slower. The root cause is a 3–5x increase in NMEA sentence volume
(primarily GSV satellite-in-view data, one sentence per 4 satellites per
constellation) that the GPS task must process on a single CPU core shared
with the display worker.

Three compounding inefficiencies make this expensive:

1. **Byte-by-byte UART reads** — `NmeaGps::read()` calls `_serial.read()`
   per byte. Each call goes through `uart_read_bytes(&b, 1, 10ms_timeout)`,
   adding syscall overhead per byte.

2. **All 7 NMEA parsers compiled and active** — libnmea-esp32 parses every
   recognized sentence (GGA, GLL, RMC, GSA, GSV, TXT, VTG) with
   `malloc()` → field parsing → `free()` per sentence. NmeaGps only uses
   3 types (GGA, RMC, GSA). The rest are parsed and discarded.

3. **9600 baud** — at peak data volume (~1000+ bytes/sec with fix), 9600
   baud (960 bytes/sec raw capacity) becomes a bottleneck. The UART is
   active nearly continuously, leaving no idle gap between 1 Hz epochs.

The GPS task runs at priority 5 (above the display worker at priority 4).
Its increased CPU usage when processing fix data preempts the display
worker during the `wait_busy_low()` poll loop, stretching the wall-clock
time of the full refresh.

## Changes

Four changes, ordered by impact. Each is independent and can be landed
separately.

### 1. Bulk UART Reads (airgradient-serial, airgradient-gps)

Add a bulk read method to the serial interface and use it in NmeaGps.

**airgradient-serial — new virtual method on `AirgradientSerial`:**

```cpp
/// Read up to @p len bytes into @p buf.  Non-blocking: returns immediately
/// with whatever is available (0 if nothing).
/// @return Number of bytes actually read.
virtual int read(uint8_t *buf, int len);
```

Default implementation: loop calling `read()` per byte (backwards-compatible
for subclasses that don't override).

**AirgradientUART override:**

```cpp
int AirgradientUART::read(uint8_t *buf, int len) {
    if (!isInitialized || buf == nullptr || len <= 0) return 0;
    int n = uart_read_bytes(_port_num, buf, len, 0);  // zero timeout
    return (n > 0) ? n : 0;
}
```

One syscall for the entire batch instead of one per byte.

**NmeaGps::read() — use bulk read:**

```cpp
bool NmeaGps::read() {
    bool got_sentence = false;
    const int avail = _serial.available();
    if (avail <= 0) return false;

    uint8_t buf[256];
    const int to_read = (avail < static_cast<int>(sizeof(buf)))
                            ? avail : static_cast<int>(sizeof(buf));
    const int n = _serial.read(buf, to_read);

    for (int i = 0; i < n; i++) {
        if (_process_byte(static_cast<char>(buf[i]))) {
            got_sentence = true;
        }
    }
    return got_sentence;
}
```

`read()` may be called multiple times per GPS task cycle (every 10 ms), so
a 256-byte local buffer is sufficient — multiple calls will drain the UART
ring buffer across consecutive iterations.

Stack cost: 256 bytes on the GPS task stack (4096 total, plenty of room).

### 2. Sentence-Level Filter (airgradient-gps)

Skip `nmea_parse()` entirely for sentence types NmeaGps does not use.

In `NmeaGps::_handle_sentence()`, before calling `nmea_parse()`:

```cpp
void NmeaGps::_handle_sentence(char *sentence, size_t length) {
    // Sentence format: $XXYYY,...  where XX = talker ID, YYY = sentence ID.
    // Only parse GGA, RMC, GSA — skip everything else (GSV, GLL, VTG, TXT).
    if (length < 6) return;
    const char *id = sentence + 3;  // skip "$XX"
    if (strncmp(id, "GGA", 3) != 0 &&
        strncmp(id, "RMC", 3) != 0 &&
        strncmp(id, "GSA", 3) != 0) {
        return;
    }

    nmea_s *parsed = nmea_parse(sentence, length, 1);
    // ... existing switch + nmea_free ...
}
```

This eliminates all `malloc()`/parse/`free()` overhead for unused
sentences. With GPS fix, this skips ~10–15 sentences per second (primarily
GSV).

The check is 3 `strncmp` calls (9 byte comparisons worst case) — negligible.

### 3. Remove Unused libnmea Parsers (libnmea-esp32)

In `components/libnmea-esp32/CMakeLists.txt`, reduce the parser list:

```cmake
set(parsers gpgga gprmc gpgsa)
```

This changes `PARSER_COUNT` from 7 to 3, removes 4 source files from
compilation (`gpgsv.c`, `gpgll.c`, `gpvtg.c`, `gptxt.c`), and shrinks the
static parser lookup array. Unrecognized sentences that somehow reach
`nmea_parse()` fail the type lookup and return `NULL` immediately.

Combined with the sentence filter (change 2), this is defense-in-depth:
unknown sentences never reach `nmea_parse()`, and even if they did, no
parser exists to handle them.

### 4. Baud Rate 115200 (products/go)

Switch the TAU1113 from its default 9600 baud to 115200 using the module's
binary command.

**Why:** at 9600 baud, the raw capacity is 960 bytes/sec. A multi-GNSS
fix can produce 1000+ bytes/sec of NMEA data. The module cannot transmit
it all within one 1 Hz epoch, forcing it to drop or delay sentences. At
115200 baud (11520 bytes/sec), there is ample headroom. The UART is active
for ~0.1s per epoch instead of nearly continuously, giving the CPU a clean
idle window.

**TAU1113 command** (from datasheet Table 26):

| Baud | Binary (hex) |
|---|---|
| 115200 | `F1 D9 06 00 08 00 00 00 00 00 00 00 C2 01 00 D1 E0` |
| 9600 | `F1 D9 06 00 08 00 00 00 00 00 00 00 80 25 00 B3 07` |

Append `0D 0A` to each command per the datasheet note.

**Boot sequence** — the GPS module stays powered during deep sleep but
loses its baud rate setting on full power-off (BMS ship mode). On boot:

```
1. Open UART at 115200
2. Read for ~200 ms
3. If valid NMEA received → done (module already at 115200 from before sleep)
4. Close UART, reopen at 9600
5. Send 115200 binary command + 0D 0A
6. Wait 100 ms
7. Close UART, reopen at 115200
```

This auto-detect handles both fresh boot (module at 9600) and deep-sleep
wake (module at 115200). No persistent state needed.

**Where:** product-specific baud negotiation function in `go_gps.cpp`,
called before `GpsSensor::begin()` in both `GpsService::run()` and
`gps_read_once()`. The shared `NmeaGps`/`GpsSensor` interface is unchanged
— it just receives a different baud rate.

**board_config.h:** change `GPS_BAUD` to `115200`. The auto-detect
function uses `GPS_BAUD` as the target and `9600` as the fallback.

## File Summary

| File | Change |
|---|---|
| `components/airgradient-serial/include/airgradient_serial.h` | Add `virtual int read(uint8_t *buf, int len)` |
| `components/airgradient-serial/include/airgradient_uart.h` | Declare `int read(uint8_t *buf, int len) override` |
| `components/airgradient-serial/airgradient_uart.cpp` | Implement bulk `read()` via `uart_read_bytes` |
| `components/airgradient-gps/drivers/nmea_gps/nmea_gps.cpp` | Bulk read in `read()`, sentence filter in `_handle_sentence()` |
| `components/libnmea-esp32/CMakeLists.txt` | `set(parsers gpgga gprmc gpgsa)` |
| `products/go/main/go_gps.cpp` | Add `negotiate_baud()`, call from `run()` |
| `products/go/main/go_gps.h` | Expose baud negotiation if needed by `gps_read_once()` |
| `products/go/main/board_config.h` | `GPS_BAUD = 115200` |

## Risks and Edge Cases

**Bulk read buffer size (256 bytes):** at 115200 baud with 10 ms task
yield, up to ~115 bytes arrive per cycle. 256 bytes covers worst-case
jitter. If the task is delayed longer (e.g. display refresh preempts it
at current priorities), the UART ring buffer (1024 bytes) absorbs the
backlog across multiple `read()` calls.

**Sentence filter correctness:** the filter matches bytes 3–5 of the raw
sentence buffer (the sentence ID portion after the 2-character talker ID).
This works for all standard talker prefixes (GP, GN, GL, GA, GB). If the
TAU1113 emits proprietary sentences (`$PCAS...`), they would also be
filtered out — which is correct since NmeaGps does not handle them.

**Baud negotiation failure:** if the 115200 command fails (module
unresponsive, wrong firmware), the auto-detect falls through to 115200
anyway, which won't work. Mitigation: add a final fallback — if no valid
NMEA at 115200 after the switch attempt, revert to 9600 and log a warning.

**Baud negotiation on fast-path boot:** `gps_read_once()` currently calls
`gps.begin(baud_rate)` directly. It needs the same auto-detect logic.
Extract the negotiation into a shared free function both call sites use.

**AirgradientSerial API compatibility:** the new bulk `read()` overload
has a different signature from the existing single-byte `read()`. C++
overload resolution handles this cleanly. Subclasses that don't override
the bulk variant get the default loop-over-single-byte implementation,
which is correct (just slower). No breakage for other serial users (e.g.
PMS5003, SenseAir).

**Existing NmeaGps tests:** the test `StubSerial` provides a single-byte
`read()`. After the change, `NmeaGps::read()` calls the bulk overload.
The default base-class implementation loops over single-byte `read()`, so
the stub works without modification. No test changes needed.

## Not Addressed

**GPS task priority:** lowering the GPS task from priority 5 to below the
display worker (priority 4) would also fix the display symptom. However,
this only masks the problem — the GPS task still wastes CPU on unused
sentence parsing. The optimizations above fix the root cause and
incidentally improve battery life by reducing total CPU-active time.

**Disabling NMEA sentences at the module:** the TAU1113 does not support
per-sentence enable/disable commands (confirmed from datasheet). The
sentence filter in NmeaGps is the equivalent software-side solution.

**UART RX buffer size increase:** the current 1024-byte buffer is
sufficient. At 115200 baud, a full 1 Hz epoch (~1400 bytes) fits
comfortably. At 9600 baud the data arrives spread over ~1 second so the
buffer never holds more than ~100 bytes at a time (drained every 10 ms).
