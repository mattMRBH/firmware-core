# airgradient-client

Unified AirGradient server client. One `AgClient` object handles config fetch,
measure post, MQTT, and CoAP traffic against the AirGradient backend over a
single transport chosen at boot.

## Status

`Experimental`

Only the WiFi HTTP path is wired up in this release. CoAP, MQTT, and the
cellular `begin()` path are present as stubs and abort if called. See
[`spec.md`](spec.md) for the full design and the future-work boundary.

## Scope

This component owns:

- AG-server URL construction and response code interpretation
- `Measures` → JSON serialization (with dual-channel averaging) for the
  HTTP path
- Protocol-client interfaces (`HttpClient`, `MqttClient`, `CoapClient`)
- WiFi HTTP backend (`WifiHttpClient`) over `esp_http_client`
- Vendored `coap-packet` and `payload-encoder` libraries (used by the
  future cellular CoAP backend)

This component does not own:

- Network bring-up (WiFi stack must be ready before `http_*` calls)
- Config parsing (returns the raw JSON body to the caller)
- OTA binary download (future `airgradient-ota` component)
- Cellular backends (future spec)

## Directory Layout

```text
components/airgradient-client/
  clients/             -- protocol client interfaces (HTTP, MQTT, CoAP)
  types/               -- NetworkType, AgClientResult, AgClientMeasuresType
  services/            -- AgClient, payload serializer, TLS cert
  backends/            -- WifiHttpClient (esp_http_client wrapper)
  lib/                 -- vendored coap-packet and payload-encoder
  tests/               -- host tests + AgClientTestAccess friend
  CMakeLists.txt
  Kconfig
  README.md
  spec.md
```

## Public Includes

```cpp
#include "services/ag_client.h"
#include "types/client_types.h"
```

The `clients/` interfaces and `backends/` are internal: callers never
instantiate them directly.

## Design

```text
caller -> AgClient -> HttpClient (interface) -> WifiHttpClient -> esp_http_client
                   -> CoapClient (interface) -> (future cellular backend)
                   -> MqttClient (interface) -> (future backend)
```

`AgClient` builds URLs, serializes measures to JSON, and maps transport +
status outcomes to `AgClientResult` (`Ok`, `BufferTooSmall`,
`TransportError`, `ServerError`, `NotRegistered`). The protocol-specific
interfaces are the mock seam for host tests.

## Usage

```cpp
AgClient client;
if (!client.begin("aabbccddeeff", NetworkType::Wifi)) {
    // handle init failure
}

// Build measures with invalid sentinels for unmeasured fields, then set
// only what you actually sampled.
AgClientMeasuresType m{};
m.co2.co2 = MeasuresInvalid::CO2;
// ... (initialize remaining fields to invalid sentinels) ...
m.temp_hum_a.temperature = 23.5f;

if (client.http_post_measures(m, -55) == AgClientResult::Ok) {
    // shipped
}
```

See [`spec.md`](spec.md) §Measures Initialization Contract for why
`AgClientMeasuresType m{}` alone is unsafe.

## Configuration

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_AG_CLIENT_MEASURES_TYPE_FULL` | `y` | Use full `Measures` variant |
| `CONFIG_AG_CLIENT_MEASURES_TYPE_BASIC` | `n` | Use `MeasuresBasic` |
| `CONFIG_AG_CLIENT_MEASURES_TYPE_AGO` | `n` | Use `MeasuresAGo` |
| `CONFIG_AG_CLIENT_CELLULAR_SUPPORT` | `n` | Reserved for future cellular work |

## Dependencies

- `components/airgradient-common/` — `Measures` types and `AG_LOG*` macros
- `esp_http_client` — WiFi HTTP backend
- `json` — bundled cJSON for JSON serialization

## Tests

Host tests live in `components/airgradient-client/tests/` and run through
the top-level [tests runner](../../tests/README.md). They use a friend
class (`AgClientTestAccess`) to inject a hand-rolled `MockHttpClient`.

## Notes

- The previous library's `airgradient-client` (in `tmp/`) is being replaced
  by this component. Once consumers migrate, `tmp/airgradient-client/` can
  be removed.
- Calling a method whose backend is not yet implemented (any `coap_*`,
  `mqtt_*`, or HTTP-on-cellular) aborts with a clear error log. This is by
  design — it surfaces programming bugs early.
