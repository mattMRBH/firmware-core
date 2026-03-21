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

- `hal/` — public BLE types and abstract interfaces (`BleCharacteristic`,
  `BleService`, `BleServer`)
- `drivers/` — NimBLE-backed concrete implementation (`NimbleBleServer`,
  `NimbleBleService`, `NimbleBleCharacteristic`)

## Design Direction

```text
product code / service -> BleServer& -> NimbleBleServer -> esp-nimble-cpp -> NimBLE stack
```

Product composition code creates a `NimbleBleServer` instance and passes a
`BleServer&` into services or tasks that need BLE access.

`esp-nimble-cpp` is a private dependency: NimBLE headers are confined to
the `drivers/` layer and are not visible to callers of this component.

## Typical Call Sequence

```cpp
NimbleBleServer ble;

ble.init("MyDevice");

BleService *svc = ble.add_service("ABCD");
BleCharacteristic *ch = svc->add_characteristic("1234", BleProperty::READ | BleProperty::NOTIFY);

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

`BleService::start()` is required. It registers the service definition and its
characteristics with the NimBLE GATT database.

`BleServer::start_advertising()` then starts the underlying `NimBLEServer`
before enabling advertising, so callers do not need a separate HAL method for
server start.

## Advertising Payload

The HAL currently exposes only the most common advertising payload fields:

- local name via `BleServer::set_advertising_name()`
- one or more advertised service UUIDs via
  `BleServer::add_advertised_service_uuid()`

Both methods must be called after `init()` and before `start_advertising()`.
