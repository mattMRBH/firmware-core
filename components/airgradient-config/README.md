# AirGradient-Config Component

This component will provide shared configuration persistence for AirGradient
firmware.

The shared responsibility is the storage mechanism, not the product settings
schema.

## Scope

`airgradient-config` should own:

- typed key-value persistence for `int`, `bool`, and `string`
- a shared storage interface for configuration backends
- an ESP-IDF NVS backend for persisted configuration
- common result semantics for storage operations

`airgradient-config` should not own:

- product-specific settings structs
- product defaults
- product-level validation rules
- remote/local config merge policy
- device-specific configuration meaning
- shared host-side config simulation unless a real shared need appears later

## Design Direction

The shared pattern is:

```text
product settings schema + shared config store
```

This means each product defines its own settings structure and explicit mapping
to keys, while the shared component provides the persistence API and backends.

## Directory Layout

```text
components/airgradient-config/
  hal/
  backends/
  tests/
  CMakeLists.txt
  README.md
```

- `hal/` - shared config persistence interfaces and public types
- `backends/` - concrete storage implementations such as NVS
- `tests/` - reserved for future shared config tests if shared config logic grows

## Minimal API Direction

The first version should stay small and explicit.

Expected operations:

- `get_int(key, out)`
- `set_int(key, value)`
- `get_bool(key, out)`
- `set_bool(key, value)`
- `get_string(key, out)`
- `set_string(key, value)`
- `erase(key)`
- `commit()`

The shared HAL now starts with:

- `hal/config_store.h` - `ConfigStore` interface and `ConfigStoreResult`

The first backend now starts with:

- `backends/nvs_config_store.*` - ESP-IDF NVS-backed implementation of
  `ConfigStore`

Optional later operations:

- `exists(key)`
- `clear()` or namespace-level erase

## Result Semantics

Storage operations should distinguish between these outcomes:

- success
- key not found
- storage/backend error

Product-level invalid values should not be treated as storage errors.

Recommended handling model:

- storage layer reports `success`, `not_found`, or `error`
- product code loads defaults first
- product code overlays stored values when present
- product code validates loaded values
- invalid loaded values fall back to product defaults for that field

## Product Responsibility

Each product should own:

- its settings struct
- default values
- field validation
- mapping between settings fields and config keys

This keeps the shared component reusable while allowing different products to
have different settings layouts.

## Example Direction

```cpp
struct ProductSettings {
    int measurement_interval_sec;
    bool led_enabled;
    std::string device_name;
};
```

The shared component should not know about this struct. Product code should map
it explicitly to stored keys using the shared `ConfigStore` interface.

## Testing Direction

At this stage, `airgradient-config` is only defining the shared storage
interface and the ESP-IDF NVS backend.

Product-specific config logic should be tested where that logic lives, most
likely in product code or in higher-level shared components that use
`ConfigStore`.
