# airgradient-provisioning

Wi-Fi provisioning manager: owns the provisioning state machine and a
captive-portal transport so product code never reimplements the Wi-Fi
onboarding flow.

## Status

`Scaffold` — checkpoint 1 of the [provisioning spec](provisioning_spec.md)
is implemented (Wi-Fi captive portal). The BLE GATT transport described
by checkpoint 2 is not yet wired; the `ble` parameter on
`ProvisioningManager::start()` is currently a nullable pointer and is
ignored.

## Scope

This component owns:

- the provisioning state machine
  (`Idle` → `WaitingForCredentials` → `Connecting` → `Connected`)
- the Wi-Fi captive-portal HTTP API and embedded portal page
- the captive DNS responder that redirects DNS queries to the AP IP
- a shared `ScanFilter` utility (dedup + RSSI sort) used by both
  transports
- a single event callback that surfaces every lifecycle transition to
  the product

This component does not own:

- `WifiManager`, `AgBleServer`, or `HttpServer` — they are borrowed
  from the product for the duration of the session
- application-level provisioning status (server reachable, monitor
  configured, etc.) — the product pushes those codes via
  `send_ble_status()` once BLE is wired in checkpoint 2
- credential storage beyond what `esp_wifi_set_config()` does via NVS
- product-specific UI/triggers for entering provisioning

## Directory Layout

```text
components/airgradient-provisioning/
  types/                     public types and enums
  services/                  public ProvisioningManager API
  internal/                  scan filter, DNS codec, transports, timer
  assets/portal.html         embedded captive-portal page
  tests/                     host tests for the pure logic
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
                              \-> CaptiveDnsResponder -> lwIP UDP
                              \-> WifiManager& (borrowed)
```

Product code constructs `WifiManager`, `AgBleServer`, and `HttpServer`,
then hands them to `ProvisioningManager::start()`. The manager wires
its internal transports onto those objects, drives the state machine,
and emits lifecycle events through a single callback. After
`stop()` returns the product resumes full ownership of the borrowed
objects.

The `ScanFilter` utility (`internal/scan_filter.h`) and the
DNS-packet codec (`internal/dns_packet.h`) are pure functions —
host-tested in isolation. The captive DNS responder wraps them in a
lwIP raw-UDP socket on hardware; under `TEST_HOST` it becomes a no-op
stub since the codec is covered separately.

## Usage

```cpp
ProvisioningManager prov;
prov.set_on_event([](const ProvisioningEventInfo &info) {
  // dispatch to product task — do not call back into the manager here
});

ProvisioningConfig cfg = {};
std::strncpy(cfg.ap.ssid, "airgradient-ABCD", sizeof(cfg.ap.ssid) - 1);
std::strncpy(cfg.ap.password, "cleanair", sizeof(cfg.ap.password) - 1);
cfg.overall_timeout_ms = 180000;

prov.start(wifi, /*ble*/ nullptr, http, cfg);
```

## Configuration

| Symbol | Default | Purpose |
| --- | --- | --- |
| `CONFIG_AG_PROVISIONING_DEFAULT_OVERALL_TIMEOUT_MS` | `0` | Default inactivity timeout (0 = disabled) |

## Dependencies

- `components/airgradient-common/` — shared types and RTOS abstraction
- `components/airgradient-wifi/` — `WifiManager` (borrowed)
- `components/airgradient-http-server/` — `HttpServer` interface
  (borrowed)
- `json` (ESP-IDF managed) — cJSON for portal request/response
  encoding
- `esp_timer`, `lwip` (ESP-IDF) — one-shot inactivity timer and
  captive DNS UDP socket

## Tests

Host tests live under
[`components/airgradient-provisioning/tests/`](tests/) and run through
the [top-level tests runner](../../tests/README.md). They cover:

- `ScanFilter` — dedup, sort, cap, RSSI floor
- DNS query parse / A-response build
- `ProvisioningManager` state-machine transitions
- portal JSON handlers via `TestHttpRequest`

The captive DNS responder and `esp_timer`-backed inactivity timer are
exercised on hardware only.
