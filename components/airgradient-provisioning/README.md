# airgradient-provisioning

Wi-Fi provisioning manager: owns the provisioning state machine and two
transports — a Wi-Fi captive portal and a BLE GATT interface — so
product code never reimplements the Wi-Fi onboarding flow.

## Status

`Experimental` — state machine, both transports (Wi-Fi captive portal
and BLE GATT), captive DNS, scan filter, and JSON credential parser are
implemented and host-tested. Exercised end-to-end on hardware via the
reference product and the Python integration tests under
`tests/integration/`. UUIDs, BLE status code allocation, and the portal
HTML/JS are still subject to change before going stable.

## Scope

This component owns:

- the provisioning state machine
  (`Idle` → `WaitingForCredentials` → `Connecting` → `Connected`)
- the Wi-Fi captive-portal HTTP API and embedded portal page
- the BLE GATT provisioning service (credential write, scan
  notifications, status notifications) and Device Information Service
- the captive DNS responder that redirects all DNS queries to the AP IP
- captive-portal OS probe URL handlers (iOS, Android, Windows, Firefox)
  that trigger the in-app captive browser via `302` redirect
- a shared `ScanFilter` utility (dedup + RSSI sort) used by both
  transports
- a shared `parse_provisioning_json()` credential parser used by both
  transports
- a single event callback that surfaces every lifecycle transition to
  the product
- Wi-Fi AP setup and teardown (ApSta on start, Sta on stop)
- HTTP server lifecycle (start on `start()`, route cleanup + optional
  stop on `stop()`)
- BLE server lifecycle (init on `start()`, deinit on `stop()`)
- BLE advertising management (stop on client connect, restart on
  disconnect)
- BLE client connect/disconnect tracking for inactivity timeout

This component does not own:

- `WifiManager`, `AgBleServer`, or `HttpServer` — they are borrowed
  from the product for the duration of the session
- application-level provisioning status (server reachable, monitor
  configured, etc.) — the product pushes those codes via
  `send_ble_status()`
- credential storage beyond what `esp_wifi_set_config()` does via NVS
- static IP persistence — `WifiStaticIpConfig` is delivered to the
  product via the event callback; the product must persist and re-apply
  it on subsequent boots
- `disable_cloud` persistence — same as static IP; the product reads
  it from the event callback and decides what it means
- product-specific UI/triggers for entering provisioning

## Directory Layout

```text
components/airgradient-provisioning/
  types/                     public types and enums
  services/                  public ProvisioningManager API
  internal/                  transports, scan filter, DNS codec, JSON parser, timer
  assets/portal.html         embedded captive-portal page
  tests/                     host tests
  CMakeLists.txt
  Kconfig
  README.md
```

- `types/` — pure C++ types, no ESP-IDF dependencies
- `services/provisioning_manager.h` — the only public header product
  code includes
- `internal/` — transport plumbing and pure-C++ utilities; not part of
  the public API
- `assets/portal.html` — self-contained HTML/CSS/JS shipped via
  `EMBED_FILES`

## Public Includes

```cpp
#include "services/provisioning_manager.h"
#include "types/provisioning_types.h"
```

## Design

```text
product -> ProvisioningManager -> WifiPortalTransport -> HttpServer&
                               \-> BleTransport --------> AgBleServer&
                               \-> CaptiveDnsResponder -> lwIP UDP
                               \-> WifiManager& (borrowed)
```

Product code constructs `WifiManager`, `AgBleServer`, and `HttpServer`,
then hands them to `ProvisioningManager::start()`. The manager wires
its internal transports onto those objects, drives the state machine,
and emits lifecycle events through a single callback. After `stop()`
returns the product resumes full ownership of the borrowed objects.

### State Machine

```text
[*] --> Idle
Idle --> WaitingForCredentials : start()
WaitingForCredentials --> Connecting : credentials submitted
Connecting --> WaitingForCredentials : connect failed (single attempt)
Connecting --> Connected : got IP
Connected --> Idle : stop()
WaitingForCredentials --> Idle : stop() or timeout
Connecting --> Idle : stop()
```

Connection attempts use `max_retry_count = 0` — a single STA attempt
per credential submission. On failure, `ConnectFailed` fires within
~6 seconds and the manager returns to `WaitingForCredentials` so the
user can retry from either transport.

### Lifecycle Events

| Event | Meaning | `data` populated? | `ip` populated? |
|---|---|---|---|
| `Started` | transports active, waiting for credentials | no | no |
| `Connecting` | credentials received, STA connect in progress | yes | no |
| `ConnectFailed` | connect attempt failed, still listening | no | no |
| `Connected` | STA has an IP address | yes | yes |
| `Stopped` | provisioning torn down | no | no |

`Stopped` includes a `stop_reason` field: `ProductRequested` (explicit
`stop()` call) or `TimedOut` (inactivity timeout expired).

### Inactivity Timeout

- Controlled by `ProvisioningConfig::overall_timeout_ms` (0 = disabled).
- Timer starts when entering `WaitingForCredentials`.
- Timer pauses when any client connects (AP STA join or BLE central
  connect).
- Timer resumes when all clients disconnect (both AP and BLE counts
  reach zero).
- On expiry: manager performs full teardown (same as `stop()`) and
  fires `Stopped` with `TimedOut` reason.

### BLE Transport

The BLE transport creates two GATT services on the borrowed
`AgBleServer`:

**AirGradient Provisioning Service** (`acbcfea8-e541-4c40-9bfd-17820f16c95c`):

| Characteristic | UUID | Properties |
|---|---|---|
| Credentials/Status | `703fa252-...` | READ, READ_ENC, WRITE, WRITE_ENC, NOTIFY |
| Wi-Fi Scan | `467a080f-...` | WRITE, WRITE_ENC, NOTIFY |

**Device Information Service** (`180A`):

| Characteristic | UUID | Properties |
|---|---|---|
| Model Number | `2A24` | READ, READ_ENC |
| Serial Number | `2A25` | READ, READ_ENC |
| Firmware Revision | `2A26` | READ, READ_ENC |
| Manufacturer Name | `2A29` | READ, READ_ENC |

BLE security uses Just Works (`NO_INPUT_NO_OUTPUT`) with bonding and
Secure Connections (`BOND | SC`).

Advertising is single-connection: stopped on client connect, restarted
on disconnect.

Scan results are sent as paginated JSON notifications (3 networks per
page, ~100 ms between pages via `ProvisioningTimer`).

### BLE Status Codes

The provisioning manager automatically sends these on Wi-Fi state
changes:

| Code | Name | Meaning |
|---|---|---|
| `0` | `WIFI_CONNECTED` | Wi-Fi connected |
| `10` | `WIFI_CONNECT_FAILED` | Failed to connect |

Products send application-level codes via `send_ble_status()`:

| Code | Name | Meaning |
|---|---|---|
| `1` | `CONNECTING_TO_SERVER` | Connecting to AirGradient server |
| `2` | `SERVER_REACHABLE` | AirGradient server is reachable |
| `3` | `MONITOR_CONFIGURED` | Monitor is configured on dashboard |
| `11` | `SERVER_UNREACHABLE` | AirGradient server is unreachable |
| `12` | `GET_CONFIG_FAILED` | Failed to get monitor config |
| `13` | `NOT_REGISTERED` | Monitor not registered on dashboard |

All codes are defined as named constants in `ProvisioningBleStatus`.

### Captive Portal Detection

The portal registers OS-specific probe URLs (`/hotspot-detect.html`,
`/generate_204`, `/connecttest.txt`, `/canonical.html`, etc.) that
return `302 Found` with `Location: http://192.168.4.1/`. This triggers
the in-app captive browser on iOS, Android, Windows, and Firefox.

The `Location` header **must** be an absolute URL — iOS CNA silently
follows relative redirects without popping the captive browser.

## Contracts

### Preconditions for `start()`

- The `HttpServer` instance must **not** be started and must **not**
  have any routes registered. The provisioning manager owns the HTTP
  server's route table and lifecycle for the duration of the session.
- The `AgBleServer` instance must **not** be initialized. The
  provisioning manager owns the BLE server's lifecycle (init/deinit)
  for the duration of the session.
- `ProvisioningConfig::ap.ssid` must not be empty.
- The event callback should be set before `start()`.

### What `start()` Does

1. Wires WifiManager callbacks (scan, connect, disconnect, AP
   client join/leave).
2. Registers all HTTP routes (portal page, API endpoints, captive
   probe URLs, favicon).
3. Initializes the BLE server, creates GATT services (provisioning
   service + DIS), sets security, configures advertising, and starts
   advertising.
4. Wires BLE transport callbacks (credentials, scan request, client
   connect/disconnect).
5. Sets Wi-Fi mode to `ApSta` and starts the soft-AP.
6. Starts the captive DNS responder on UDP port 53.
7. Starts the HTTP server on `config.http_port`.
8. Arms the inactivity timeout (if configured).
9. Emits `Started`.

### What `stop()` Does

1. Cancels the inactivity timeout.
2. Stops the captive DNS responder.
3. Clears BLE transport callbacks, then calls `teardown()` (which
   calls `ble.deinit()`).
4. Calls `http.unregister_all()` — wipes all registered routes.
5. Optionally calls `http.stop()` (default `true`; pass `false` to
   keep the server running for product routes).
6. Detaches all WifiManager callbacks.
7. Sets Wi-Fi mode to `Sta` — drops the AP, preserves any active STA
   association.
8. Emits `Stopped`.

### What `stop(false)` Does Differently

Same as `stop()` except the HTTP server stays running (not stopped).
Routes are still wiped. The product can immediately register its own
routes on the same server without a bind/unbind cycle.

### Destruction

The destructor calls `stop()` as a safety net but **clears the event
callback first** to prevent the `Stopped` event from firing into
potentially-destroyed captures. If the product needs the `Stopped`
event, it must call `stop()` explicitly before the manager goes out
of scope.

### Callback Threading

Callbacks fire from various task contexts (WiFi event loop, HTTP
server task, NimBLE task, esp_timer task). Callers must not block
inside callbacks or call back into `ProvisioningManager`. Signal your
own task and do heavy work there.

### What the Product Must Persist

The provisioning manager does not persist anything beyond what
`esp_wifi_set_config()` stores automatically (SSID + password). The
following fields are delivered via the `Connected` event and the
product is responsible for saving and re-applying them:

| Field | Where | Product responsibility |
|---|---|---|
| `data.static_ip` | `ProvisioningEventInfo` | Save to NVS; call `wifi.set_static_ip()` on every boot before `connect()` |
| `data.disable_cloud` | `ProvisioningEventInfo` | Save to NVS; honor in application logic |

### DHCP Timeout

`ProvisioningConfig::connect_timeout_ms` (default 15000 ms) is
forwarded to `WifiManager::set_dhcp_timeout_ms()`. This bounds how
long the manager waits for DHCP after L2 association — the only
failure mode ESP-IDF does not surface natively.

## Usage

```cpp
EspWifiHal wifi_hal;
WifiManager wifi(wifi_hal);
wifi_hal.init();
IdfHttpServer http;
NimbleBleServer ble;

ProvisioningManager prov;
prov.set_on_event([&](const ProvisioningEventInfo &info) {
    // fire-and-forget signal to product task — do not block here
    switch (info.event) {
    case ProvisioningEvent::Connected:
        // info.data.ssid, info.data.disable_cloud, info.data.static_ip
        // info.ip — STA IP address
        break;
    case ProvisioningEvent::Stopped:
        // info.stop_reason — ProductRequested or TimedOut
        break;
    default:
        break;
    }
});

ProvisioningConfig cfg = {};
std::strncpy(cfg.ap.ssid, "airgradient-ABCD", sizeof(cfg.ap.ssid) - 1);
std::strncpy(cfg.ap.password, "cleanair", sizeof(cfg.ap.password) - 1);
cfg.ble.device_name = "AirGradient";
cfg.ble.model_name  = "I-9PSL";
cfg.overall_timeout_ms = 180000;

prov.start(wifi, ble, http, cfg);
// ... wait for Connected or Stopped via event callback ...
prov.send_ble_status(ProvisioningBleStatus::CONNECTING_TO_SERVER);
prov.stop();
// Wi-Fi is STA-only; HTTP stopped; BLE deinit'd; product resumes control.
```

See
[`products/reference/main/test_provisioning.cpp`](../../products/reference/main/test_provisioning.cpp)
for the full hardware smoke test.

## Dependencies

- `components/airgradient-common/` — shared types and RTOS abstraction
- `components/airgradient-ble/` — `AgBleServer` interface (borrowed)
- `components/airgradient-wifi/` — `WifiManager` (borrowed)
- `components/airgradient-http-server/` — `HttpServer` interface
  (borrowed)
- `json` (ESP-IDF managed) — cJSON for portal and BLE JSON
  encoding/decoding
- `esp_timer`, `lwip` (ESP-IDF) — one-shot inactivity timer, scan
  pagination timer, and captive DNS UDP socket

## Tests

Host tests live under
[`components/airgradient-provisioning/tests/`](tests/) and run through
the [top-level tests runner](../../tests/README.md). They cover:

- `ScanFilter` — dedup, sort, cap, RSSI floor
- DNS query parse / A-response build
- `provisioning_json` — shared credential JSON parsing (ssid, password,
  disableCloud, staticIp), strict static IP validation, error enum
- `BleTransport` — GATT setup/teardown, credential write callback,
  scan pagination via timer, status notifications, advertising
  stop/start on connect/disconnect, teardown during active connection
- `ProvisioningManager` state-machine transitions (start, stop,
  connect, fail, retry, timeout, teardown verification), BLE
  integration (dual-transport credentials, scan result fanout,
  `send_ble_status()`, BLE client timeout pause/resume, mixed AP + BLE
  client timeout suppression)
- portal JSON handlers via `TestHttpRequest` (scan, provision, status,
  captive probe redirect)
- `stop()` teardown contract (routes wiped, HTTP server stopped, BLE
  deinit'd, Wi-Fi reverted to STA)

The captive DNS responder and `esp_timer`-backed timers are exercised
on hardware only.

## Notes

- The captive-portal probe redirect uses a hardcoded
  `http://192.168.4.1/` in the `Location` header. This is the lwIP
  soft-AP default gateway IP. If the AP subnet is ever reconfigured,
  this must be updated in
  `internal/wifi_portal_transport.cpp :: handle_captive_probe()`.
- BLE scan pagination uses `ProvisioningTimer` (wraps `esp_timer` on
  firmware, no-op with `fire_for_test()` under `TEST_HOST`). Pages are
  sent at ~100 ms intervals to avoid overwhelming the BLE stack.
- The provisioning manager clears BLE transport callbacks before
  calling `teardown()` to prevent the disconnect triggered by
  `AgBleServer::deinit()` from calling back into the manager while the
  mutex is held.
