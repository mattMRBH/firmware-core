# Local Server API V1 — Contract and Naming Decision

> **Decision doc for team review.** It compares the **legacy** local-server API
> contract with the **proposed V1** redesign, and recommends a naming policy.
> Once the team agrees, the outcome folds into the component spec
> ([`spec.md`](spec.md)) and the implementation. This file is temporary and is
> deleted after the decision is recorded.

## Purpose

The new device models on this codebase ship a versioned local API (`/api/v1`).
We need a team decision on **field naming**: keep the legacy wire vocabulary, or
adopt a new convention. This doc lays out the contracts side by side and makes a
recommendation so the discussion is concrete.

## Background

- The legacy local server (AirGradient ONE / Open Air, fw >= 3.0.10) exposes
  `GET /measures/current`, `GET /config`, and `PUT /config`. Field names are an
  established, camelCase-ish vocabulary (`rco2`, `atmp`, `pm02`, `pmStandard`).
- Tens of thousands of devices already speak that vocabulary. Consumers include
  the Home Assistant integration **and** custom scripts/services that pull
  directly over HTTP.
- V1 is a **structural** redesign (versioned paths, settings vs. commands split,
  flat optional config, structured errors, discovery via `api` mDNS TXT,
  coexistence with legacy devices). None of those structural changes require
  renaming fields.

## Naming Principle (decided)

> Use **camelCase** for JSON fields, **kebab-case** for URL path segments, **keep
> the legacy vocabulary where it works**, and **rename names that mislead or are
> unreasonably opaque** — not names that are merely terse-but-guessable.

Path segments are kebab-case (the prevailing REST convention: `/actions/calibrate-co2`);
JSON body and field names are camelCase. The two layers use different conventions
by design, not by accident.

Rationale:

- **Versioning manages necessary breaking changes; it does not justify
  gratuitous ones.** Bumping to V1 is not a reason to swap a naming convention
  that has no defect. camelCase stays.
- **Field names are a long-lived consumer contract.** Where a legacy name is
  clear enough, keeping it lets consumers reuse parsing code across versions.
  A wholesale vocabulary swap forces every consumer to maintain two field maps
  for no functional gain.
- **Consistency over the product line and over time outweighs internal tidiness.**
  `snake_case` would be internally neat but ecosystem-inconsistent; the legacy
  vocabulary, though a little ad hoc, is widely known and documented.
- **Misleading or opaque names are a real defect** and a version boundary is the
  right place to fix them. The discipline: rename what misreads _or_ what a new
  developer cannot reasonably guess without documentation; keep what is
  terse-but-known.
- **Negative booleans are a recurring cost, not a one-time learning cost.** A
  terse name is looked up once; a negative boolean (`disableFoo: false`) forces
  mental inversion on every read. The version boundary is the right place to
  flip polarity.

This rejects two extremes: full `snake_case` (gratuitous fragmentation) and
"never touch anything" (carries forward genuinely confusing names).

## Design Rationale

### Why the rename threshold is "opaque," not just "misleading"

An earlier draft of this doc proposed renaming **only** `pm02` — the one field
that actively misreads. Everything else was "terse but known." We widened the
bar after two observations:

1. **The rename cost is lower than it appears.** Legacy consumers are not
   broken by V1 renames — they keep using `/measures/current` with the old
   names. V1 is a new URL, new error format, new optional-field behavior.
   Any consumer adopting V1 is already writing new code. The marginal cost of
   also learning a new field name is near zero compared to the structural
   adaptation.

2. **The "known" audience is the wrong audience to optimize for.** Existing
   power users already know `rco2` and `atmp`. They aren't helped or hurt by
   a rename (they already know both meanings). The audience that pays the cost
   of a bad name is **every future developer** encountering the API for the
   first time — and that audience grows forever while the legacy audience is
   fixed. Optimizing for new-developer clarity has compounding returns.

The discipline remains: don't rename for tidiness. Rename when a new developer
**cannot reasonably guess the meaning** without reading a field reference. By
that bar, `rco2` (what is "r"?), `atmp` (not a standard abbreviation), `rhum`
(non-standard abbreviation; `humidity` is universal), `serialno` (casual
abbreviation; `serialNumber` is standard), `wifi` (RSSI? channel? band?
boolean?), and `disableCloudConnection` (negative boolean) qualify. `tvocIndex`,
`pm003Count`, and `abcDays`-domain fields do not — they're terse but guessable in
context. `temp` and `humidity` are renamed as a pair: renaming temperature for
clarity while leaving `rhum` terse would read as jarring side by side.

### Why camelCase, not snake_case

We considered switching to `snake_case` for V1 (common in modern JSON APIs:
Stripe, AWS, GitHub). We rejected it:

- **The readability gain is marginal.** `tvocIndex` vs `tvoc_index`,
  `pm003Count` vs `pm003_count` — the difference is real but small. It does
  not justify fragmenting the vocabulary across versions.
- **camelCase is a perfectly valid JSON convention** (Google APIs, Azure,
  Firebase). There is no industry-wide winner.
- **Cross-version field reuse is a real convenience.** A curl user or Grafana
  dashboard that parses `tvocIndex` from a legacy device can use the same
  field name on a V1 device. That zero-cost migration path disappears with a
  convention switch. The fields we _do_ rename have a clear reason beyond
  casing.
- **If V2 ever ships, it inherits camelCase.** Switching conventions mid-product
  creates a permanent split: "legacy and V1 are camelCase, V2 is snake_case."
  Staying consistent avoids that.

The convention is camelCase. Individual renames happen for cause, not for
style.

### Why `corrections` keys follow V1 naming, not legacy

The cloud config sends corrections keyed by legacy measure names:

```json
"corrections": { "pm02": { ... }, "atmp": { ... }, "rhum": { ... } }
```

V1 local uses the renamed keys instead:

```json
"corrections": { "pm25": { ... }, "temp": { ... }, "humidity": { ... } }
```

Rationale:

- **The local V1 API should be self-consistent.** A user who reads `pm25` in
  measures and then has to write `corrections.pm02` in config must know two
  vocabularies within the same API version. That defeats the purpose of the
  renames.
- **The cloud API is not a consistency target.** The AirGradient public API
  (`api.airgradient.com/public/api/v1/`) already mixes conventions internally
  (`tvocIndex` vs `tvoc_index` in route measures, `firmware` vs
  `firmwareVersion`, camelCase fields with `_corrected` snake_case suffixes).
  There is no consistent legacy vocabulary to preserve — each contract
  (cloud config, public API, local legacy, local V1) is its own surface.
- **The firmware mapping cost is trivial.** Three key renames during
  serialization for the local V1 response, easily tested. The firmware
  already maps between cloud vocabulary and internal representation; adding
  a V1 serialization path is the same pattern.
- **`corrections` is writable locally.** If it were read-only pass-through
  from the cloud, legacy keys would be defensible. But since users configure
  corrections via the V1 local API, they should use V1 vocabulary.

## Structural Changes (not naming — for context)

| Area | Legacy | V1 |
|---|---|---|
| Paths | `/measures/current`, `/config` | `/api/v1/measures`, `/api/v1/config` |
| Versioning | none (evolves in place) | version in path + `api` mDNS TXT |
| Commands | boolean config fields (`co2CalibrationRequested`) | `POST /api/v1/actions/<kebab-id>` |
| Config GET | all fields always present | flat, only supported fields present |
| Config PUT | partial, single field | partial, multiple fields; `204` on success |
| Errors | `text/plain` | structured JSON `{ "error": { code, field, message } }` |
| Unknown PUT key | ignored | rejected `400` |
| Discovery | `_airgradient._tcp` mDNS | same + `api=1` TXT for version routing |

## Measures Contract

`GET /api/v1/measures` (legacy `GET /measures/current`).

| Legacy field | V1 field | Type | Unit / meaning | Naming decision |
|---|---|---|---|---|
| `serialno` | **`serialNumber`** | String | device serial number | **rename** — casual abbreviation; `serialNumber` is standard |
| `wifi` | **`wifiRssi`** | Number | WiFi RSSI (dBm) | **rename** — `wifi` is ambiguous (RSSI? channel? band?) |
| `boot` | `boot` | Number | measurement-cycle counter (resets on restart) | keep |
| `bootCount` | — | Number | deprecated duplicate of `boot` | **drop** (legacy marked deprecated) |
| `firmware` | `firmware` | String | firmware version | keep |
| `model` | `model` | String | model name | keep |
| `rco2` | **`co2`** | Number | CO2 (ppm) | **rename** — vestigial `r` prefix; `co2` is universally clear |
| `pm01` | `pm01` | Number | PM1.0 (µg/m³) | keep |
| `pm02` | **`pm25`** | Number | PM2.5 (µg/m³) | **rename** — `pm02` misreads as PM0.2/PM2 |
| `pm10` | `pm10` | Number | PM10 (µg/m³) | keep |
| `pm003Count` | `pm003Count` | Number | 0.3µm particle count /dL | keep |
| `atmp` | **`temp`** | Number | temperature (°C) | **rename** — non-standard abbreviation; `temp` is universal |
| `rhum` | **`humidity`** | Number | relative humidity (%) | **rename** — non-standard abbreviation; pairs with `temp` |
| `tvocIndex` | `tvocIndex` | Number | VOC index | keep |
| `tvocRaw` | `tvocRaw` | Number | VOC raw | keep |
| `noxIndex` | `noxIndex` | Number | NOx index | keep |
| `noxRaw` | `noxRaw` | Number | NOx raw | keep |
| `atmpCompensated` | — | Number | corrected temperature | **drop** (unused) |
| `rhumCompensated` | — | Number | corrected humidity | **drop** (unused) |
| `pm02Compensated` | — | Number | corrected PM2.5 | **drop** (unused) |
| `pm01Standard` / `pm02Standard` / `pm10Standard` | — | Number | standard-particle PM | **defer** (not exposed yet) |
| `pm005Count` / `pm01Count` / `pm02Count` / `pm50Count` / `pm10Count` | — | Number | other particle counts | **defer** |
| `ledMode` | — | String | current LED mode | **drop** (config domain, not a measure) |
| `monitorDisplayCompensatedValues` | — | Boolean | display-compensation flag | **drop** (config domain) |

V1 emits a field **only when valid**; an unsupported or currently-invalid field
is **omitted** (no `null`).

Example V1 response:

```json
{
  "serialNumber": "ecda3b1eaaaf",
  "wifiRssi": -46,
  "boot": 6,
  "firmware": "4.0.0",
  "model": "I-9PSL",
  "co2": 447,
  "pm01": 3,
  "pm25": 7,
  "pm10": 8,
  "pm003Count": 442,
  "temp": 25.8,
  "humidity": 43,
  "tvocIndex": 100,
  "tvocRaw": 33051,
  "noxIndex": 1,
  "noxRaw": 16307
}
```

## Config Contract

`GET` / `PUT /api/v1/config`.

| Legacy field | V1 field | Type | Values / range | Naming decision |
|---|---|---|---|---|
| `country` | `country` | String | ISO-3166 alpha-2 | keep |
| `pmStandard` | `pmStandard` | String | `ugm3` / `us-aqi` | keep |
| `temperatureUnit` | `temperatureUnit` | String | `c` / `f` | keep |
| `ledBarMode` | **`ledMode`** | String | `co2` / `pm` / `iaqs` / `off` | **rename** — more generic; not always a bar |
| `ledBarBrightness` | `ledBarBrightness` | Number | 0–100 | keep |
| `displayBrightness` | `displayBrightness` | Number | 0–100 | keep |
| `abcDays` | **`co2AbcDays`** | Number | 0–200 (default 8) | **rename** — clarifies this is CO2-specific |
| `tvocLearningOffset` | `tvocLearningOffset` | Number | 0–720 (default 12) | keep |
| `noxLearningOffset` | `noxLearningOffset` | Number | 0–720 (default 12) | keep |
| `configurationControl` | `configurationControl` | String | `both` / `local` / `cloud` | keep |
| `postDataToAirGradient` | **`postDataToCloud`** | Boolean | post measurement data to cloud | **rename** — decouple from vendor name |
| `disableCloudConnection` | **`cloudConnection`** | Boolean | master cloud switch (`true` = connected) | **rename + flip polarity + make writable** (legacy read-only, negative) |
| `mqttBrokerUrl` | `mqttBrokerUrl` | String | MQTT broker URL | keep |
| `httpDomain` | `httpDomain` | String | custom HTTP domain | keep |
| `offlineMode` | — | Boolean | offline mode (boot-only) | **defer** |
| `model` | — | String | hardware id (GET only) | **drop** from config (already in measures) |
| `corrections` | `corrections` | Object | correction algorithms; inner keys follow V1 naming (`pm25`, `temp`, `humidity`) | keep (keys renamed to match V1 measures) |

Commands move out of config to actions:

| Legacy config field | V1 action |
|---|---|
| `co2CalibrationRequested` | `POST /api/v1/actions/calibrate-co2` |
| `ledBarTestRequested` | `POST /api/v1/actions/test-leds` |

## The One Functional Addition

The legacy `disableCloudConnection` is **read-only** (changeable only from the
Wi-Fi setup page). V1 renames it to `cloudConnection`, **flips the polarity**
(`true` = cloud on), and makes it **writable** over the local API. It remains
the master switch and overrides `configurationControl` and `postDataToCloud`:
when `false`, all cloud activity (data post, cloud config, automatic OTA) is
off. On AirGradient Go this maps to the existing `disableCloud` setting
(inverted).

## Summary of Naming Changes

The decided policy yields ten renames plus structural moves:

- **Rename (misleading):** `pm02` → `pm25`.
- **Rename (opaque):** `rco2` → `co2`, `atmp` → `temp`, `rhum` → `humidity`,
  `serialno` → `serialNumber`, `wifi` → `wifiRssi`.
- **Rename (overly specific):** `ledBarMode` → `ledMode`.
- **Rename (clarify scope):** `abcDays` → `co2AbcDays`.
- **Rename (vendor-specific):** `postDataToAirGradient` → `postDataToCloud`.
- **Rename + flip polarity + make writable:** `disableCloudConnection` →
  `cloudConnection` (`true` = connected).
- **Include in V1:** `mqttBrokerUrl`, `httpDomain`, `corrections` (were deferred).
- **Drop (unused / deprecated / config-domain):** `bootCount`, `*Compensated`,
  `ledMode` (measures), `monitorDisplayCompensatedValues`.
- **Move to actions (kebab path):** `co2CalibrationRequested` →
  `calibrate-co2`, `ledBarTestRequested` → `test-leds`.
- **Everything else: unchanged from legacy.**

## Resolved Decisions

1. **`pm02` → `pm25`**: ✅ Rename only `pm02`. Keep `pm01` and `pm10` as-is
   (they are unambiguous in context).
2. **Clarity renames**: ✅ `rco2` → `co2` (opaque prefix), `atmp` → `temp`
   (non-standard abbreviation), `rhum` → `humidity` (pairs with `temp`),
   `serialno` → `serialNumber` (casual abbreviation), `wifi` → `wifiRssi`
   (ambiguous). Keep `tvocIndex`, `noxIndex`, `pm003Count` (terse but
   guessable).
3. **`disableCloudConnection`**: ✅ Rename to `cloudConnection`, flip polarity
   (`true` = connected), make writable.
4. **Config renames**: ✅ `ledBarMode` → `ledMode` (generic),
   `abcDays` → `co2AbcDays` (clarify scope),
   `postDataToAirGradient` → `postDataToCloud` (decouple vendor).
5. **Action paths**: ✅ kebab-case path segments (`/actions/calibrate-co2`,
   `/actions/test-leds`); JSON fields stay camelCase.
6. **`mqttBrokerUrl`, `httpDomain`, `corrections`**: ✅ Include in V1.
   `corrections` inner keys follow V1 measure naming (`pm25`, `temp`,
   `humidity`), not legacy (`pm02`, `atmp`, `rhum`). Cloud is not a consistency
   target.
7. **Remaining deferred fields**: ✅ `offlineMode`, standard-particle PM, and
   extra particle counts stay out of V1 for now.
8. **`configurationControl: "both"`**: ✅ Keep value as-is (no rename).
9. **Naming convention**: ✅ camelCase. No convention switch to snake_case.
