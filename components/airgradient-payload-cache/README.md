# AirGradient-Payload-Cache Component

This component provides a shared payload cache for AirGradient firmware.

It keeps cached payloads derived from the shared measures types and persists
them through a small storage HAL so the queue logic stays host-testable.

## Scope

`airgradient-payload-cache` should own:

- a bounded FIFO payload queue
- a shared payload cache type derived from the common measures structs
- a storage HAL for persisting cached payloads
- an ESP-IDF RTC-backed storage implementation

`airgradient-payload-cache` should not own:

- payload upload logic
- network retry policy
- product-specific measurement scheduling
- sensor reading orchestration

## Directory Layout

```text
components/airgradient-payload-cache/
  hal/
  services/
  backends/
  CMakeLists.txt
  README.md
```

- `hal/` - shared payload cache storage interface and public payload types
- `services/` - queue behavior independent from ESP-IDF storage details
- `backends/` - concrete storage implementations such as RTC retained memory

## Public API Direction

The public payload type stays intentionally simple through a single alias:

```cpp
typedef ... PayloadCacheType;
```

The payload cache can be configured through Kconfig:

- `CONFIG_PAYLOAD_CACHE_MAX_SIZE` controls the fixed ring-buffer slot count
- `CONFIG_PAYLOAD_CACHE_TYPE_FULL` stores `Measures`
- `CONFIG_PAYLOAD_CACHE_TYPE_BASIC` stores `MeasuresBasic`

The default payload type is `Measures`.

The current queue design follows the reference payload cache closely:

- fixed-size ring buffer
- `head` and `tail` indices
- overwrite-oldest behavior when the queue becomes full
- `N - 1` usable capacity so `head == tail` continues to mean empty

## Build Integration

- ESP-IDF builds this component through
  `components/airgradient-payload-cache/CMakeLists.txt`
- product code should depend on the service and inject a storage backend
- host-side tests can mock the storage HAL without compiling the RTC backend
