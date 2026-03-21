# Components

This folder contains shared AirGradient ESP-IDF components used across
AirGradient product firmware.

Each component should own one clear responsibility and stay reusable across
different product models.

This folder may also contain third-party or vendor code when that code is used
as part of a shared component. For example, a reusable sensor driver from a
third party can live here if it is part of the shared firmware foundation.

Current components:

- `airgradient-ble/` - shared BLE peripheral HAL and NimBLE-backed driver for
  GATT server, characteristic management, and advertising control
- `airgradient-nand-storage/` - shared SPI NAND flash HAL providing FATFS
  filesystem mount/unmount lifecycle for application-level POSIX I/O
- `airgradient-touch/` - shared capacitive touch HAL and CAP1203 3-channel
  driver with noise detection and recalibration support
- `airgradient-common/` - shared data types and common runtime utilities such as
  `Measures` types and RTOS abstraction
- `airgradient-config/` - shared configuration persistence foundation for typed
  key-value storage and reusable config backends
- `airgradient-gpio/` - shared GPIO abstraction and ESP-IDF-backed GPIO driver
  implementation for reusable pin control and interrupt registration
- `airgradient-payload-cache/` - shared cached payload queue service and RTC
  retained storage backend for `Measures`-derived payloads
- `airgradient-sensors/` - sensor HAL interfaces, concrete sensor drivers, and
  shared sensor orchestration such as `SensorManager`
- `airgradient-serial/` - shared serial transport abstractions and
  implementations used by sensors and other hardware integrations
- `ads1115/` - reusable ADS1115 ADC helper component used by sensor drivers such
  as AlphaSense

Guidelines:

- keep product-specific BSP and application wiring outside this folder
- place shared capability code here only when it is reusable across products
- prefer capability-focused components over generic catch-all components
- it is fine for a component to contain third-party or vendor code if that code
  belongs to the shared component responsibility

As the firmware grows, new shared capabilities such as connectivity, display,
or power can follow the same pattern and live alongside these components.
