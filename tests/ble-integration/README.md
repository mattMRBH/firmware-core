# AGo BLE Integration Tests

Hardware integration tests for the AirGradient Go BLE service. Connects to a
real AGo device over Bluetooth Low Energy and verifies the GATT protocol:
service discovery, CBOR payloads, data types, notification flows, and the
history download state machine.

## Prerequisites

- Python >= 3.11
- Linux with BlueZ (or macOS with CoreBluetooth)
- An AGo device advertising in **Portable** mode
- The device must already be **bonded/paired** with the host machine

## Install

```bash
pip install -e tests/ble-integration
```

## Run

```bash
# Auto-scan for a device whose name starts with "AGo-"
pytest tests/ble-integration/ -v

# Specify a device address explicitly
pytest tests/ble-integration/ -v --ago-address AA:BB:CC:DD:EE:FF

# Run a single test file
pytest tests/ble-integration/test_measures.py -v
```

### CLI Options

| Option | Default | Description |
|---|---|---|
| `--ago-address` | _(auto-scan)_ | BLE address or name of the AGo device |
| `--ago-scan-timeout` | `10` | Seconds to scan before giving up |
| `--ago-notify-timeout` | `30` | Max seconds to wait for a notification |

## Test Overview

### `test_service_discovery.py` — GATT Profile (3 tests)

Verifies the AGo service UUID and all four characteristic UUIDs are present
with the correct properties (read/write/notify).

### `test_measures.py` — Measures Characteristic (6 tests)

Subscribes to Measures notifications once (module-scoped fixture), captures a
single notification, then validates it across all tests:

- A notification arrives within the timeout
- Payload is valid CBOR decoding to a map
- `"ts"` (unix timestamp) is always present
- All keys belong to the known set (`t`, `h`, `pm1`, `pm25`, `pm10`, `co2`,
  `tvoc`, `nox`, `pres`, `lat`, `lon`, `alt`, `fix`, `sat`, `ts`)
- Each field has the correct CBOR type (float, uint, float64)
- GPS fields follow the grouping rule: `fix`+`sat` always appear together;
  position fields only appear when the fix group is present

### `test_status.py` — Status Characteristic (7 tests)

Reads the Status characteristic once (module-scoped fixture), then validates
the payload across all tests:

- Payload is a 10-key CBOR map with all expected keys
- Field types match the spec (uint, float, str, bool)
- `"charging"` is a known enum string
- `"bat_pct"` is in 0-100 range
- `"bat_v"` is non-negative
- `"fw"` is a non-empty string

### `test_config.py` — Config Characteristic (9 tests)

Covers read, write, and notify operations:

- **Read** (5 tests, sync): reads Config once (module-scoped fixture), then
  validates 12 config keys present with correct types; `gps_mode` and `op_mode`
  are valid enum strings
- **Set config** (async): writes `{"op": "set", ...}`, verifies the device
  sends a Config notification with `"type": "config"` and all 13 keys (12
  config + type discriminator)
- **Roundtrip** (async): toggles `temp_f`, re-reads to confirm the change,
  then restores the original value
- **Notify field types** (async): triggers a Config notification and verifies
  all field types in the notification payload
- **Command** (async): writes `{"op": "cmd", "cmd": "co2_cal"}`, verifies the
  `cmd_result` notification format (success or failure)

### `test_history.py` — History Characteristic (8 tests)

Exercises the full download protocol. Tests that require stored sessions are
**skipped** when the device reports zero sessions.

- **List**: writes `{"op": "list"}`, verifies `"sessions"` array with
  `id`/`pts`/`ts` per entry
- **Start download**: verifies `"started"` response with `session`, `total`,
  `pt_size=55`
- **Binary format**: validates tag `0x01`, uint16 LE point index, and 55-byte
  RoutePointWire struct layout
- **Done count**: `"done"` response `"sent"` matches `"started"` `"total"`
- **End**: `"ended"` response after `{"op": "end"}`
- **Errors**: invalid session ID returns `"session_not_found"`; `fill` without
  active download returns `"no_active_download"`

## File Structure

```
tests/ble-integration/
  pyproject.toml              Dependencies and pytest config
  conftest.py                 Fixtures: scan, connect, notification collectors
  ago_protocol.py             UUIDs, CBOR key sets, type maps, encode/decode
  test_service_discovery.py   GATT service and characteristic verification
  test_measures.py            Measures notification tests
  test_status.py              Status read tests
  test_config.py              Config read/write/notify tests
  test_history.py             History download flow tests
```

## Design Notes

- **Single connection**: the BLE connection is session-scoped (one connect for
  the entire pytest run, disconnect at teardown).
- **Session-scoped event loop**: all async fixtures and tests share a single
  session-scoped asyncio event loop (`asyncio_default_fixture_loop_scope` and
  `asyncio_default_test_loop_scope` both set to `"session"` in
  `pyproject.toml`). This is required because the BLE client (and its
  underlying BlueZ D-Bus connection) is session-scoped — async operations on
  any other event loop would fail with a loop mismatch error.
- **Shared read fixtures**: read-only tests (Measures, Status, Config read) use
  module-scoped fixtures that fetch data once and share it across all tests in
  the module. This avoids redundant BLE I/O and keeps validators synchronous.
- **Per-test notification fixtures**: interactive tests (Config write, History)
  use function-scoped `NotificationCollector` fixtures that
  subscribe/unsubscribe per test.
- **Non-destructive config tests**: original settings are read, modified, then
  restored after each test.
- **No security handling**: assumes the device is already bonded. If not bonded,
  authenticated reads/writes will fail with a bleak error.
