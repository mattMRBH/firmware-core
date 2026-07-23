# AGo Local Server Integration Tests

Hardware integration tests for the AirGradient Go Local Server API. The suite
connects to a real device in Stationary mode and verifies mDNS discovery, HTTP
routes, JSON schemas, configuration admission, structured errors, actions, and
OTA access policy.

## Prerequisites

- Python >= 3.11
- An AirGradient Go connected to the same network as the test host
- The device running in **Stationary** mode
- Multicast DNS available between the host and device, or the device IP address

## Install

```sh
pip install -e products/go/tests/local-server-integration
```

## Run

Auto-discover one Go device through `_airgradient._tcp.local.`:

```sh
pytest products/go/tests/local-server-integration/ -v
```

Target a device by URL or serial number:

```sh
pytest products/go/tests/local-server-integration/ -v \
  --ago-url http://192.168.1.20

pytest products/go/tests/local-server-integration/ -v \
  --ago-serial aabbccddeeff
```

Run one test file:

```sh
pytest products/go/tests/local-server-integration/test_measures.py -v \
  --ago-url http://192.168.1.20
```

### CLI Options

| Option | Default | Description |
|---|---|---|
| `--ago-url` | _(mDNS)_ | Explicit Local Server base URL |
| `--ago-serial` | _(any)_ | Select and validate one serial number |
| `--ago-discovery-timeout` | `10` | mDNS discovery timeout in seconds |
| `--ago-http-timeout` | `5` | Per-request HTTP timeout in seconds |
| `--ago-convergence-timeout` | `20` | Config convergence timeout in seconds |
| `--ago-allow-config-write` | off | Enable persisted toggle and restoration |
| `--ago-allow-calibration` | off | Enable physical CO2 calibration |
| `--ago-ota-active` | off | Confirm committed OTA is active |

### Viewing HTTP Traffic

Requests and responses are logged at `DEBUG` level under the
`ago_local_api_test` logger:

```sh
pytest products/go/tests/local-server-integration/ -v \
  --ago-url http://192.168.1.20 --log-cli-level=DEBUG
```

## Safety

The default suite does not change durable configuration. It submits only an
empty configuration update, malformed requests, and the unsupported LED action.

Persisted mutation tests require `--ago-allow-config-write`. They toggle only
`temperatureUnit`, poll the asynchronous GET snapshot for convergence, and
restore the original value during fixture teardown. They skip when
`configurationControl` is `cloud`.

CO2 calibration is physical and fire-and-forget over HTTP. It runs only with
`--ago-allow-calibration`; the HTTP response confirms dispatch, not completion.
Do not run it without appropriate calibration conditions.

Do not run this suite with `pytest-xdist`. Configuration tests assume exclusive
access to one device.

## Test Overview

### `test_discovery.py` — mDNS Profile

Verifies the `_airgradient._tcp.local.` service, Go model, API version, hostname,
TXT identity, port, and agreement with `GET /api/v1/measures`.

### `test_measures.py` — Measures Snapshot

Validates required identity fields, optional measurement fields, numeric ranges,
unknown-key rejection, and omission of invalid values rather than JSON `null`.

### `test_config.py` — Configuration

Validates the complete Go configuration schema and safe empty-update admission.
The opt-in round-trip test toggles and restores `temperatureUnit`.

### `test_errors.py` — Error Contract

Exercises empty, malformed, non-object, and trailing request bodies; unknown and
invalid fields; nested dotted error paths; and known fields unsupported by Go.

### `test_actions.py` — Actions

Checks the model-specific `test-leds` response. The opt-in calibration test
verifies that `calibrate-co2` returns an empty `200` fire-and-forget response.

### `test_ota.py` — OTA Policy

These tests require an operator or dedicated test build to stage and hold a
committed Stationary Wi-Fi OTA. The suite does not start OTA itself. Run only
this file while OTA is active:

```sh
pytest products/go/tests/local-server-integration/test_ota.py -v \
  --ago-serial aabbccddeeff --ago-ota-active
```

They verify cached GET availability, read-only PUT/action policy, malformed-body
precedence, and retained mDNS advertisement. A successful OTA may reboot before
the suite completes, so use an abortable or intentionally failing OTA for this
check. Add `--ago-allow-calibration` only when explicitly testing the calibration
route's OTA policy; this protects against accidental calibration if OTA ends
before that request arrives.

## File Structure

```text
products/go/tests/local-server-integration/
  pyproject.toml       Dependencies and pytest configuration
  conftest.py          Discovery, HTTP, opt-in, and restoration fixtures
  ago_local_api.py     Routes, schema validators, and response assertions
  test_discovery.py    mDNS and identity tests
  test_measures.py     Measures schema tests
  test_config.py       Config read, no-op, and round-trip tests
  test_errors.py       Parsing and provider errors
  test_actions.py      Fire-and-forget action tests
  test_ota.py          Operator-gated OTA policy tests
```

## Design Notes

- **One device per run:** mDNS discovery fails on ambiguity unless
  `--ago-serial` or `--ago-url` selects the target.
- **Synchronous requests:** the firmware owns one HTTP task, so tests avoid
  unnecessary client concurrency.
- **Asynchronous configuration:** `202` means admitted. Mutating tests poll GET
  for convergence before proceeding.
- **Explicit interaction:** physical calibration and OTA tests are skipped
  unless their opt-in flags are present.
- **No action completion over HTTP:** BLE and UI report calibration completion;
  the Local Server API remains fire-and-forget.
