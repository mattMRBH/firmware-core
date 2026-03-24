# AirGradient-BLE Component

This component provides a generic BLE peripheral (GATT server) HAL for
AirGradient firmware. It wraps the `esp-nimble-cpp` vendor library behind
clean abstract interfaces so that product code and services depend only on
the HAL, not on NimBLE directly.

## Responsibilities

- BLE stack initialisation and teardown
- GATT service and characteristic registration
- BLE advertising control
- BLE advertising payload setup for local name and service UUIDs
- Connect and disconnect event delivery via callbacks
- Write event delivery per characteristic via callbacks
- Characteristic value updates and notifications to connected clients

## Directory Layout

```text
components/airgradient-ble/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` — public BLE types and abstract interfaces (`AgBleCharacteristic`,
  `AgBleGattService`, `AgBleServer`)
- `drivers/` — NimBLE-backed concrete implementation (`NimbleBleServer`,
  `NimbleBleGattService`, `NimbleBleCharacteristic`)

## Design Direction

```text
product code / service -> AgBleServer& -> NimbleBleServer -> esp-nimble-cpp -> NimBLE stack
```

Product composition code creates a `NimbleBleServer` instance and passes a
`AgBleServer&` into services or tasks that need BLE access.

`esp-nimble-cpp` is a private dependency: NimBLE headers are confined to
the `drivers/` layer and are not visible to callers of this component.

## Typical Call Sequence

```cpp
NimbleBleServer ble;

ble.init("MyDevice");

AgBleGattService *svc = ble.add_service("ABCD");
AgBleCharacteristic *ch = svc->add_characteristic("1234", AgBleProperty::READ | AgBleProperty::NOTIFY);

svc->start();

ble.set_connect_callback([](uint16_t h) { /* ... */ });
ble.set_disconnect_callback([](uint16_t h, int reason) { /* ... */ });
ble.set_advertising_name("MyDevice");
ble.add_advertised_service_uuid("ABCD");
ble.start_advertising();

// Push updates to connected clients:
ch->set_value(data, len);
ch->notify();
```

## Service and Server Start

`AgBleGattService::start()` is required. It registers the service definition and its
characteristics with the NimBLE GATT database.

`AgBleServer::start_advertising()` then starts the underlying `NimBLEServer`
before enabling advertising, so callers do not need a separate HAL method for
server start.

## Advertising Payload

The HAL currently exposes only the most common advertising payload fields:

- local name via `AgBleServer::set_advertising_name()`
- one or more advertised service UUIDs via
  `AgBleServer::add_advertised_service_uuid()`

Both methods must be called after `init()` and before `start_advertising()`.

## Security

The HAL provides optional security configuration for pairing, bonding, and
link encryption. If `set_security()` is never called, the server operates
without security (connections are unauthenticated and unencrypted).

### Configuration

Call `set_security()` after `init()` and before `start_advertising()` to
select the IO capability and authentication requirements:

```cpp
ble.set_security(AgBleIoCapability::DISPLAY_ONLY,
                 AgBleAuth::BOND | AgBleAuth::MITM);
```

Available IO capabilities: `DISPLAY_ONLY`, `DISPLAY_YES_NO`, `KEYBOARD_ONLY`,
`NO_INPUT_NO_OUTPUT`, `KEYBOARD_DISPLAY`.

Available auth flags (combinable with `|`): `AgBleAuth::BOND` (persist keys),
`AgBleAuth::MITM` (man-in-the-middle protection), `AgBleAuth::SC` (LE Secure
Connections).

### Passkey Pairing Call Sequence

```cpp
NimbleBleServer ble;
ble.init("MyDevice");

// Configure security: display-only with bonding + MITM.
ble.set_security(AgBleIoCapability::DISPLAY_ONLY,
                 AgBleAuth::BOND | AgBleAuth::MITM);

// Service with characteristics that require authentication.
BleService *svc = ble.add_service("ABCD");
BleCharacteristic *ch = svc->add_characteristic(
    "1234", AgBleProperty::READ | AgBleProperty::READ_AUTHEN |
            AgBleProperty::WRITE | AgBleProperty::WRITE_AUTHEN);
svc->start();

// Passkey callback: called when a pairing peer needs to see the passkey.
ble.set_passkey_display_callback([](uint32_t passkey) {
    // Display the 6-digit passkey to the user (e.g., on a screen).
    printf("Passkey: %06" PRIu32 "\n", passkey);
});

// Auth-complete callback: called when pairing finishes.
ble.set_auth_complete_callback([](uint16_t conn, bool ok) {
    printf("Auth %s (conn %u)\n", ok ? "OK" : "FAIL", conn);
});

ble.set_advertising_name("MyDevice");
ble.add_advertised_service_uuid("ABCD");
ble.start_advertising();
```

### Characteristic Security Properties

Characteristics can require encryption or authentication for read/write
access via the `AgBleProperty` flags:

| Flag | Meaning |
|---|---|
| `READ_ENC` | Read requires an encrypted link |
| `READ_AUTHEN` | Read requires an authenticated (MITM) link |
| `WRITE_ENC` | Write requires an encrypted link |
| `WRITE_AUTHEN` | Write requires an authenticated (MITM) link |

### Bond Management

`delete_all_bonds()` erases all stored pairing keys. Useful for factory reset
or development.

Bond persistence requires `CONFIG_BT_NIMBLE_NVS_PERSIST=y` in the product
`sdkconfig.defaults`.
