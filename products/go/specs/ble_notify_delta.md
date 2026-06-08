# BLE Notify Delta Payloads

> **This is a spec.** It describes how a feature **will be built**, not what
> currently exists. Once the feature ships, the BLE docs
> ([`products/go/docs/ble_service.md`](../docs/ble_service.md) and the client
> contract [`go_ble_client.md`](../go_ble_client.md)) become the source of
> truth and this file is deleted. See [`docs/STYLE.md`](../../../docs/STYLE.md)
> → "Doc Lifecycle".

Decouple BLE NOTIFY payloads from the READ snapshot so notifications carry
only what changed. A BLE notification is a single ATT PDU capped at `MTU − 3`
and cannot be fragmented, so the current "notify the whole snapshot" model
will break as the Config characteristic grows. This change makes NOTIFY
bounded and independent of field count for both the Config and Status
characteristics, while keeping READ/Read-Long as the authoritative full
snapshot.

## Problem

The Config and Status characteristics each hold one stored value. Today
`notify_config()` and `notify_status()` write the **full** snapshot with
`set_value()` and then call the no-arg `notify()`, which transmits that same
stored buffer. Consequences:

- **NOTIFY is MTU-bound and grows with the field count.** READ can exceed the
  MTU via the Read Long procedure (multiple PDUs, up to the 512-byte ATT
  attribute ceiling), but a notification is a single PDU bounded by `MTU − 3`
  with no continuation. The current Config notify is ~145–207 B; adding "a lot
  more" config fields will push it past the notifiable limit, where the stack
  cannot deliver the full payload (the exact oversize behavior is stack-defined).
- **NOTIFY and READ are physically the same bytes.** With only the no-arg
  `notify()`, a notification cannot be smaller than the stored READ value.
- **Status re-sends all 9 keys** on every tracking transition even though only
  `tracking` and `session` change.

The root issue is transport coupling, not field validation or value ranges.

## Goals

- NOTIFY payload size is **independent of the total number of config fields**.
- Every NOTIFY for Config and Status fits within a conservative single-PDU
  budget that holds at the minimum app-supported negotiated MTU (185 B).
- READ/Read-Long remains the authoritative **full** snapshot for both
  characteristics.
- Adding a new config field is a **single registry entry** (plus its
  decode/validate, which are out of scope here), with full-encode and
  delta-encode both driven from that one entry.
- Config READ snapshot has headroom to grow (raise the stored-value buffer to
  the 512-byte ATT ceiling).

## Non-Goals

- No change to per-field decode/validation semantics; multi-field `set` is
  intentionally restricted (single config key per write).
- No change to the Measures or History characteristics.
- No new characteristics or services; GATT layout is unchanged.
- READ pagination beyond the 512-byte ATT ceiling (deferred until a field
  actually pushes the full snapshot past 512 B).
- Unifying the config **decode** and **validate** paths into the registry
  (future work; see Open Questions).
- The existing "adopt-before-validate" / ignored `save_go_settings()` return
  behavior in the orchestrator config-set path (tracked separately).

## Design

### Transport model

| Channel | Source | Content |
|---|---|---|
| READ / Read-Long | stored value (`set_value`) | full snapshot |
| NOTIFY | buffer passed to `notify(data, len)` | delta, or command progress/result |

The enabler is a new HAL overload `notify(const uint8_t* data, size_t len)`
that transmits an arbitrary buffer **without** touching the stored value. The
stored value stays the full snapshot (written by `update_config()` /
`update_status()`), so a READ arriving on the BLE host task between the
orchestrator's writes always returns the full, current snapshot — no race, no
restore step.

#### Single-writer invariant

For each characteristic, the stored value is written through **exactly one**
method — `update_config()` for Config, `update_status()` for Status — and that
method always writes the **full snapshot**. Every notification path uses
`notify(data, len)` and never calls `set_value()` directly:

| Characteristic | Stored value (READ) | Notifications via `notify(data, len)` |
|---|---|---|
| Config | `update_config()` — full snapshot | `notify_config` delta, `notify_command_progress`, `notify_command_result` |
| Status | `update_status()` — full 9-key snapshot | `notify_status` transition delta |

To make this invariant impossible to violate from the call site, the `notify_*`
methods refresh the snapshot **internally** before sending the delta:
`notify_config(prev, cur)` calls `update_config(cur)` first, then
`notify(delta)`; `notify_status(...)` calls `update_status(...)` first, then
`notify(delta)`. This guarantees the stored value is current before any delta
goes out (closing the READ-vs-notify race) without the orchestrator having to
order two calls. The orchestrator therefore drops the redundant `update_*` call
that currently sits next to each `notify_*`; standalone `update_*` remains for
value-only steady-state refreshes.

This is a behavior change for the command notifications:
`notify_command_progress()` and `notify_command_result()` currently do
`set_value()` + `notify()`, which leaves a `cmd_progress` / `cmd_result` map in
the stored value — so a READ after a command would return that instead of the
config snapshot. Under this invariant they switch to `notify(data, len)`, and
the Config stored value remains the full config at all times. The Config
characteristic therefore carries three notification kinds (config delta,
`cmd_progress`, `cmd_result`) but only ever **reads** as the config snapshot.

### HAL change (`components/airgradient-ble`)

```cpp
class AgBleCharacteristic {
public:
  virtual bool set_value(const uint8_t *data, size_t len) = 0;
  virtual bool notify() = 0;                                  // sends stored value
  virtual bool notify(const uint8_t *data, size_t len) = 0;   // NEW: sends given buffer
  virtual void set_write_callback(AgBleWriteCallback callback) = 0;
};
```

`notify(data, len)` is a new pure-virtual, so **all three** implementers must be
updated or the build breaks:

| Implementer | Location | Behavior |
|---|---|---|
| `NimbleBleCharacteristic` | `components/airgradient-ble/drivers/nimble_ble_server.{h,cpp}` | maps to NimBLE `NimBLECharacteristic::notify(value, length)` |
| `MockBleCharacteristic` (Go) | `products/go/tests/go_ble.tests.cpp` | records notified payload separately from stored value |
| `MockBleCharacteristic` (provisioning) | `components/airgradient-provisioning/tests/mock_ble.h` | same record semantics |

Explicit mock semantics, so tests can assert READ bytes != NOTIFY bytes:

- `notify()` (no-arg) records the **current stored value** as the notified
  payload.
- `notify(data, len)` records the **supplied buffer** as the notified payload
  and leaves the stored value unchanged.

### Config field registry (`go_ble.cpp`)

A single table drives both full-encode and delta-encode. Adding a field means
adding one row and its two small helpers.

```cpp
struct ConfigField {
  const char *key;
  void (*encode_value)(CborEncoder &map, const GoSettings &s);
  bool (*differs)(const GoSettings &a, const GoSettings &b);
};

static const ConfigField CONFIG_FIELDS[] = {
    {BLE_KEY_MEAS_INT,  enc_meas_int,  dif_meas_int},
    {BLE_KEY_TEMP_F,    enc_temp_f,    dif_temp_f},
    // ... one row per config field ...
    {BLE_KEY_TOUCH_LED, enc_tled,      dif_tled},
};
static constexpr size_t CONFIG_FIELD_COUNT =
    sizeof(CONFIG_FIELDS) / sizeof(CONFIG_FIELDS[0]);
```

Full snapshot (READ) — emits every field, no discriminator:

```cpp
size_t BleService::encode_config(uint8_t *buf, size_t buf_size, const GoSettings &s) {
  CborEncoder encoder, map;
  cbor_encoder_init(&encoder, buf, buf_size, 0);
  cbor_encoder_create_map(&encoder, &map, CONFIG_FIELD_COUNT);
  for (const auto &f : CONFIG_FIELDS) {
    cbor_encode_text_stringz(&map, f.key);
    f.encode_value(map, s);
  }
  cbor_encoder_close_container(&encoder, &map);
  return cbor_encoder_get_buffer_size(&encoder, buf);
}
```

The `include_type_discriminator` parameter added in the previous refactor is
**removed**: READ never carries `"type"`, and NOTIFY is produced by the delta
encoder below, which writes `"type"` itself.

Delta (NOTIFY) — `"type":"config"` plus only the changed fields:

```cpp
size_t BleService::encode_config_delta(uint8_t *buf, size_t buf_size,
                                       const GoSettings &prev, const GoSettings &cur) {
  size_t changed = 0;
  for (const auto &f : CONFIG_FIELDS) {
    if (f.differs(prev, cur)) {
      changed++;
    }
  }

  CborEncoder encoder, map;
  cbor_encoder_init(&encoder, buf, buf_size, 0);
  cbor_encoder_create_map(&encoder, &map, changed + 1); // +1 for "type"

  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_CONFIG);

  for (const auto &f : CONFIG_FIELDS) {
    if (f.differs(prev, cur)) {
      cbor_encode_text_stringz(&map, f.key);
      f.encode_value(map, cur);
    }
  }
  cbor_encoder_close_container(&encoder, &map);
  return cbor_encoder_get_buffer_size(&encoder, buf);
}
```

`notify_config()` refreshes the snapshot through `update_config()` first, then
sends the delta — so the stored READ value is always current and the notify
never touches it:

```cpp
void BleService::notify_config(const GoSettings &prev, const GoSettings &cur) {
  update_config(cur);            // sole writer of the stored snapshot (READ)
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }
  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_config_delta(buf, sizeof(buf), prev, cur);
  if (len == 0) {
    return;                      // encoder overflow guard (see below)
  }
  assert(len <= BLE_NOTIFY_MAX_BYTES); // guard, not a wire branch
  _config_char->notify(buf, len);
}
```

### Config NOTIFY decision

`notify_config(prev, cur)` sends the delta directly — there is **no runtime
fallback**. The delta is kept within one PDU by bounding it **at the source**:
a config change can only ever touch one field per event, so the largest possible
delta is a single field.

- **BLE `set` is restricted to a single config key per write.** A `set`
  carrying more than one recognized config key is rejected before any value is
  applied, with `{"type":"cmd_result","cmd":"set","ok":false,"err":"single_field_only"}`
  — the same reject-before-adopt path already used for `unknown_config_key`.
  See "Single-field `set` enforcement" below for the counting rules.
- **Device-side changes are expected to be single-field, not guaranteed.**
  `apply_to_settings()` rewrites every field, so the single-field property holds
  only because the delta is computed from `prev` vs `cur` and the UI normally
  changes one setting per `SettingsChanged` event. The budget guard (below)
  catches any path that violates this; per-path host tests pin the expectation.
- A `set` that changes nothing yields `{"type":"config"}` (no config keys); the
  client treats that as a no-op.
- **Ordering is internal:** `notify_config(prev, cur)` calls `update_config(cur)`
  first (refreshing the authoritative snapshot), then sends the delta. The
  orchestrator just calls `notify_config(prev, cur)` and drops the separate
  `update_config` it currently calls alongside; there is no call-site ordering
  to get wrong and no READ-vs-notify race.

The largest single-field delta is `dev_name` at 64 chars ≈ 88 B, comfortably
within one PDU at the device's preferred 256-byte MTU and at the 185-byte
minimum negotiated MTU.

#### Budget as a guard, not a wire branch

`BLE_NOTIFY_MAX_BYTES` (conservative, sized to the 185-byte minimum negotiated
MTU) bounds **every** notification on these characteristics — config delta,
`cmd_progress`, `cmd_result`, and the Status transition delta — not just deltas
(hence the name, not `..._DELTA_...`). It is retained **only** as a correctness
guard, not as a runtime decision: a debug assert plus host-test invariants that
every produced notification payload is within budget. This converts any future
over-budget payload (a new large field, a batch UI/onboarding path that diffs
multiple fields, a longer error string) from a _silent on-wire failure_ into a
_loud debug/test failure_, at zero release happy-path cost.

Client rule (documented in the contract): the Config characteristic carries
three notification kinds, dispatched by the `"type"` key — `config` (delta),
`cmd_progress`, and `cmd_result`. A Config **delta** notify (`type:"config"`)
carries the changed key(s); merge it into the local model. READ remains
authoritative — re-read the full snapshot on connect.

### Single-field `set` enforcement

`decode_config_write()` gains an explicit result field:

```cpp
struct BleConfigDecodeResult {
  // ...existing fields...
  bool   has_unknown_keys = false;        // unrecognized key present
  size_t recognized_config_key_count = 0; // count of actual config keys in a "set"
};
```

Counting and classification rules:

- `recognized_config_key_count` increments **only** in the 12 config-key
  branches, counting **occurrences** (so a duplicated key also pushes the count
  past one).
- `op`, `cmd`, and the deprecated keys (`pm_int`, `other_int`, `disp_int`) do
  **not** count and do **not** flag unknown — deprecated keys remain accepted,
  ignored, and compatibility-only.
- **Aiding keys are command arguments, not config.** `lat`, `lon`, `alt`,
  `pos_acc`, `epoch`, `time_acc` belong to the `op:"cmd"` / `set_aiding`
  payload. Under `op:"set"` they are meaningless. Detection must use an explicit
  **parser flag**, not value inspection — sentinels and zero defaults
  (`pos_acc=0`, `time_acc=0`, alt-only, invalid lat/lon) make "is the field
  populated?" lossy. Set `saw_aiding_key = true` in each aiding-key branch, then
  a post-loop check (`op` resolves only after the parse loop) flags
  `op == Set && saw_aiding_key` as `has_unknown_keys`. Aiding keys remain valid
  under `op:"cmd"`.

Orchestrator rejection, in precedence order (first match wins):

1. `has_unknown_keys` → `err:"unknown_config_key"` (existing behavior, checked
   first).
2. `recognized_config_key_count > 1` → `err:"single_field_only"`.

Both reject **before** any value is adopted, mirroring the existing
unknown-key path. A new constant `BLE_VAL_ERR_SINGLE_FIELD_ONLY =
"single_field_only"` is added to `go_ble_protocol.h`.

### Status NOTIFY

Status notifies only on tracking transitions (unchanged). The delta carries the
two fields that change:

```cpp
void BleService::notify_status(const PowerSnapshot &power, const GpsData &gps,
                               bool tracking, uint32_t session_id) {
  // Refresh the full 9-key snapshot through the sole writer (READ stays full).
  update_status(power, gps, tracking, session_id);

  if (!_connected.load() || _status_char == nullptr) {
    return;
  }
  // NOTIFY carries only the transition delta.
  uint8_t delta[STATUS_DELTA_BUF_SIZE];
  size_t len = encode_status_transition(delta, sizeof(delta), tracking, session_id);
  if (len == 0) {
    return;
  }
  assert(len <= BLE_NOTIFY_MAX_BYTES);
  _status_char->notify(delta, len);
}
```

`notify_status()` routes the snapshot write through `update_status()` (the sole
writer) rather than calling `set_value()` directly, honoring the single-writer
invariant. `encode_status_transition()` emits `{tracking, session}` (2 keys);
Status has no `"type"` discriminator because the characteristic carries only
status notifications.

### READ buffer headroom

Add a dedicated `CONFIG_SNAPSHOT_BUF_SIZE = 512` for the full Config snapshot
(`update_config()` / `encode_config()`), matching the 512-byte ATT attribute
ceiling that Read-Long can serve. The general `CBOR_BUF_SIZE` (256) is retained
for everything else — Status (9 keys), Measures, History, and the small Config
delta — to avoid enlarging every stack buffer.

#### Encoder overflow detection

The encoders currently return `cbor_encoder_get_buffer_size()` unconditionally,
which on a too-small buffer would expose truncated/invalid CBOR. All CBOR
encoders (`encode_config`, `encode_config_delta`, `encode_status`,
`encode_status_transition`, and the command encoders) must check
`cbor_encoder_get_extra_bytes_needed(&encoder) != 0` (TinyCBOR's overflow
signal) and return `0` (with a log), so callers skip the write/notify rather
than emit a corrupt payload. A host-test invariant asserts the max-size full
Config snapshot — every field at maximum, `dev_name` at 64 chars — encodes to a
non-zero length ≤ `CONFIG_SNAPSHOT_BUF_SIZE`.

### Read Blob fragmentation is automatic but non-atomic

Read-Long is served by the NimBLE host, not the application. The GATT read
handler (`NimBLEServer::handleGattEvent`,
[`NimBLEServer.cpp`](../../../components/esp-nimble-cpp/src/NimBLEServer.cpp))
appends the **entire** stored value with `os_mbuf_append` on every read access
and lets the ATT server slice it by offset: the first Read Request returns
`MTU − 1` bytes, and each Read Blob Request returns the next `MTU − 1` bytes
until a short response ends the procedure. The application only has to keep the
full snapshot in the stored value via `set_value()`; no app-level paging is
needed up to the 512-byte ATT ceiling.

The caveat: the handler rebuilds the value from the **current** stored value on
every blob continuation, so if `update_config()` overwrites the snapshot
mid-read the client can stitch a **torn read** (early chunks from the old
value, later chunks from the new one). Risk is low — config changes are
infrequent and user-initiated, writes run on the orchestrator task while reads
run on the host task, and a multi-PDU read completes in milliseconds — but the
window only opens once the snapshot exceeds one PDU, which is exactly what
raising the buffer to 512 enables. Accept it for now; revisit if torn reads are
observed in the field.

### MTU is a client contract

The single-PDU budget assumes the negotiated ATT MTU is large enough. The
firmware does **not** enforce this: `MIN_USEFUL_MTU = 128` exists in
`go_ble.cpp` with a comment about suppressing notifications, but it is
referenced nowhere — there is no negotiated-MTU tracking, and `MAX_NOTIFY_PAYLOAD
= 244` is a fixed "assumes 247 MTU" constant. A central that stays at the
23-byte ATT default would not receive even an 88 B delta.

This is treated as a **client responsibility**, documented in
[`go_ble_client.md`](../go_ble_client.md) §11: the app must negotiate an MTU
≥ 185 B before subscribing to or relying on Config/Status notifications (the
existing spec already recommends ≥ 251 B). Device-side MTU tracking and
notification suppression below budget — wiring up the vestigial
`MIN_USEFUL_MTU` intent — is out of scope here and left as an Open Question.

### Wire format summary

| Characteristic | READ value | NOTIFY value |
|---|---|---|
| Config | full snapshot, no `"type"` | config delta `{"type":"config", <changed key>}`; or `{"type":"cmd_progress", ...}` / `{"type":"cmd_result", ...}` |
| Status | full 9-key snapshot | `{tracking, session}` |

Config READ always returns the config snapshot regardless of which notification
kind was last sent (single-writer invariant).

## Implementation Plan

1. **HAL overload.** Add pure-virtual `notify(const uint8_t*, size_t)` to
   `AgBleCharacteristic`; implement in `NimbleBleCharacteristic`; add to **both**
   the Go and provisioning `MockBleCharacteristic` with the record semantics
   above. (Build breaks until all three are updated.)
2. **Decouple all Config notifications.** Switch `notify_command_progress()`
   and `notify_command_result()` from `set_value()` + `notify()` to
   `notify(data, len)`, so the Config stored value is written only by
   `update_config()`.
3. **Config registry + encoders.** Introduce `CONFIG_FIELDS` and the per-field
   helpers; rewrite `encode_config()` to loop the table and drop the
   `include_type_discriminator` parameter; add `encode_config_delta()`.
4. **Config notify (internal update).** Rework `notify_config()` to
   `notify_config(prev, cur)`: call `update_config(cur)` internally first, then
   send the delta via `notify(data, len)`; add `CONFIG_SNAPSHOT_BUF_SIZE = 512`;
   add `BLE_NOTIFY_MAX_BYTES` as a debug-assert/test guard only. At both
   orchestrator call sites (`apply_settings_change`, BLE config-set path) pass
   `previous_settings` and **remove** the now-redundant separate `update_config`
   call.
5. **Encoder overflow guard.** Make all CBOR encoders return `0` (and log) on
   TinyCBOR overflow (`cbor_encoder_get_extra_bytes_needed() != 0`) instead of
   returning a truncated length.
6. **Single-field `set` enforcement.** Add `recognized_config_key_count` and a
   `saw_aiding_key` flag to `decode_config_write()` with the
   counting/classification rules above (including the post-loop
   aiding-key-under-`set` check); add `BLE_VAL_ERR_SINGLE_FIELD_ONLY =
   "single_field_only"`; reject in the orchestrator before adoption,
   `unknown_config_key` taking precedence.
7. **Status notify (internal update).** Add `encode_status_transition()`; rework
   `notify_status()` to call `update_status()` internally for the snapshot, then
   send the 2-key delta via `notify(data, len)`; remove the redundant
   orchestrator `update_status` calls beside `notify_status`.
8. **Docs.** Update [`go_ble_client.md`](../go_ble_client.md) §6 (Status), §7.4
   (Config), §7.7 (dispatch by `type`), and §11 (MTU ≥ 185 client requirement)
   for delta semantics, the single-field `set` rule, command notifications still
   arriving on Config, the merge model, and "READ on connect for baseline";
   update [`docs/ble_service.md`](../docs/ble_service.md) notify descriptions and
   the method table.
9. **Tests.** Unit, mock, stubs, and integration updates (below).

## Testing Strategy

- **Host unit (`tests/go_ble.tests.cpp`):**
  - `encode_config()` emits `CONFIG_FIELD_COUNT` keys and no `"type"`.
  - `encode_config_delta()` emits `"type":"config"` plus only the changed keys;
    a single-field change yields a 2-key map.
  - `notify_config()` sends via `notify(data, len)` and leaves the stored READ
    value as the full snapshot (assert READ bytes != NOTIFY bytes).
  - `notify_config()` refreshes the stored snapshot (via internal
    `update_config()`) before notifying, so a READ right after returns the new
    full snapshot, not the old one.
  - `notify_command_progress()` and `notify_command_result()` send via
    `notify(data, len)` and do **not** overwrite the Config READ value.
  - Every notification payload is within `BLE_NOTIFY_MAX_BYTES` (guard
    invariant): largest single-field config delta (`dev_name`, 64 chars),
    `cmd_progress`, `cmd_result` with the longest error string
    (`factory_reset_failed`), and the Status transition delta.
  - `encode_config()` of a max-size snapshot (all fields max, `dev_name` 64
    chars) returns non-zero and ≤ `CONFIG_SNAPSHOT_BUF_SIZE`; a deliberately
    undersized buffer makes the encoder return `0` (overflow guard).
  - A no-op `set` (nothing changed) emits exactly `{"type":"config"}`.
  - A multi-key (and duplicate-key) `set` is rejected with
    `err="single_field_only"` and no value applied; an unknown key still wins
    precedence with `err="unknown_config_key"`.
  - An aiding key under `op:"set"` is rejected as `unknown_config_key`, while
    the same key under `op:"cmd"` (`set_aiding`) is accepted.
  - `notify_status()` notifies `{tracking, session}` only while the stored READ
    value stays at 9 keys, and refreshes that snapshot before notifying.
- **Orchestrator (`tests/go_orchestrator.tests.cpp`):**
  - After a BLE config-set and a `SettingsChanged` event, a Config READ returns
    the updated snapshot (confirms the internal-update refresh).
  - Each UI `SettingsChanged` event path produces a within-budget delta
    (pins the device-side single-field expectation).
- **Mock:** both `MockBleCharacteristic` implementations record the last
  notified payload distinctly from the stored value, per the semantics above.
- **Stubs:** signature updates in `go_app_stubs.cpp`,
  `go_orchestrator_stubs.cpp`.
- **Integration (`tests/ble-integration/`):** set one field, assert the Config
  notify contains only that key plus `"type"`, then READ and assert the full
  snapshot; assert a two-key `set` is rejected; issue a command and assert a
  follow-up Config READ still returns the config snapshot (not the
  `cmd_result`); verify a tracking transition notify carries only
  `{tracking, session}` while a Status READ returns all 9 keys.
- **Firmware build:** `idf.py -C products/go build`.

## Open Questions

- Exact value of the `BLE_NOTIFY_MAX_BYTES` guard bound (proposed:
  conservative ~180 B to fit the 185-byte minimum negotiated MTU).
- Device-side MTU enforcement (wiring up the vestigial `MIN_USEFUL_MTU` to track
  the negotiated MTU and suppress sub-budget notifications) — deferred; MTU is a
  client contract for now.
- Should the config **decode** and **validate** paths fold into the same
  registry (one row fully describes a field)? Larger change; deferred.
- Whether to fix the orchestrator "adopt-before-validate" / ignored
  `save_go_settings()` return in the same effort or track it separately.
- Long-term READ pagination once the full Config snapshot can exceed the
  512-byte ATT ceiling.
- Whether non-atomic long reads of the Config snapshot need mitigation (e.g.
  snapshot-on-first-blob) if torn reads are observed once the snapshot spans
  multiple PDUs.
