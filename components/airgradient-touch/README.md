# AirGradient-Touch Component

This component provides a HAL for capacitive touch sensing, with the CAP1203
3-channel capacitive touch controller (Microchip Technology) as the concrete
driver. The driver is a clean re-implementation from the chip datasheet; no
vendor library is used.

## Responsibilities

- I2C device registration and identity verification (product/manufacturer ID)
- Device configuration: sensitivity, per-channel thresholds, enabled channels,
  interrupt channel mask
- Touch status reads: which channels are currently touched
- Noise flag reads (optional capability)
- Manual recalibration trigger (optional capability)
- Interrupt line acknowledgement (driver-specific, not in HAL)

## Directory Layout

```text
components/airgradient-touch/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` — `TouchChannel` flags, `TouchData` struct, `CapTouchSensor` abstract
  interface with feature-check methods
- `drivers/` — `CAP1203` concrete driver with `Config` struct

## HAL Feature Checks

The HAL follows the same optional-capability pattern as `PMSensor`:

| Method | Default | CAP1203 |
|---|---|---|
| `supports_noise()` | `false` | `true` |
| `supports_calibration()` | `false` | `true` |
| `calibrate(mask)` | no-op `false` | writes `REG_CALIBRATION_ACTIVATE` |

`out.noise` in `TouchData` is only meaningful when `supports_noise() == true`.

## Design Direction

```text
product code -> CapTouchSensor& -> CAP1203 -> i2c_master -> CAP1203 chip
```

Product code creates a `CAP1203` instance and passes a `CapTouchSensor&` to
any service that only needs to read touch state. Code that also needs
`clear_interrupt()` holds a `CAP1203&` directly.

`esp_driver_i2c` is a private dependency: ESP-IDF I2C types are confined to
the `drivers/` layer and are not visible to callers of this component.

## Typical Usage

```cpp
// Product BSP / app_main wiring (firmware-only)
CAP1203 touch(i2c_bus);   // default address 0x28, default Config

if (!touch.init()) {
    // handle failure
}

// Polling loop
TouchData data;
if (touch.read(data)) {
    if (data.touched & TouchChannel::CH1) { /* CH1 touched */ }
    if (touch.supports_noise() && data.noise) { /* noise on some channel */ }
}

// Interrupt-driven — call from GPIO interrupt handler context
touch.clear_interrupt();

// Recalibrate all channels
if (touch.supports_calibration()) {
    touch.calibrate(TouchChannel::ALL);
}
```

## CAP1203 Register Map

| Constant | Address | Description |
|---|---|---|
| `REG_MAIN_CONTROL` | `0x00` | INT bit (bit 0) |
| `REG_GENERAL_STATUS` | `0x02` | Touch event latch |
| `REG_SENSOR_INPUT_STATUS` | `0x03` | Per-channel touch flags |
| `REG_NOISE_FLAG_STATUS` | `0x0A` | Per-channel noise flags |
| `REG_SENSITIVITY_CONTROL` | `0x1F` | delta_sense \[7:4\], base_shift \[3:0\] |
| `REG_SENSOR_INPUT_ENABLE` | `0x21` | Channel enable mask |
| `REG_CALIBRATION_ACTIVATE` | `0x26` | Recalibration trigger mask |
| `REG_INTERRUPT_ENABLE` | `0x27` | Per-channel interrupt mask |
| `REG_THRESHOLD_CH1–3` | `0x30–0x32` | Per-channel threshold (0–127) |
| `REG_PRODUCT_ID` | `0xFD` | Expected: `0x6D` |
| `REG_MANUFACTURER_ID` | `0xFE` | Expected: `0x5D` |
| `REG_REVISION` | `0xFF` | Logged at init, not validated |
