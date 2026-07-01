# AirGradient Go Feature Overview

> This is a product knowledge document for non-engineering team members. It
> explains what AirGradient Go can do in practical product language, without
> requiring firmware or hardware implementation knowledge.

AirGradient Go is a portable air quality monitor for measuring personal air
quality while moving or while placed in one location. It combines environmental
sensors, GPS route tracking, an e-paper display, battery operation, BLE app
connectivity, Wi-Fi support, local storage, LEDs, and buzzer feedback.

## Product Summary

AirGradient Go is designed for people who want to understand the air quality
around them throughout the day. It can be carried outdoors, used during travel,
or placed in a fixed location. The device can work with a phone app over BLE,
connect to Wi-Fi in stationary use, or operate without radios in Offline mode.

Key product ideas:

- Portable air quality monitoring with built-in battery.
- Live on-device readings on a low-power e-paper display.
- GPS-based route tracking for mobile measurements.
- Secure BLE connection to a phone for live data, settings, and history export.
- Wi-Fi connectivity for stationary operation.
- Local storage for route and measurement history.
- Visual and audio feedback through LEDs and buzzer, where hardware supports it.

## What It Measures

AirGradient Go measures the main air quality and environmental values expected
from a portable monitor.

| Category | Measurement | User-Facing Meaning |
|---|---|---|
| Particulate Matter | PM1.0, PM2.5, PM10 | Fine particles in the air, including smoke, dust, and pollution |
| CO2 | Carbon dioxide in ppm | Ventilation and indoor air freshness indicator |
| Gases | TVOC index, NOx index | General indicators for volatile organic compounds and nitrogen oxides |
| Climate | Temperature, humidity | Ambient comfort and sensor context |
| Pressure | Barometric pressure, altitude estimate | Environmental context and altitude support |
| Location | GPS latitude, longitude, altitude, fix quality, satellites | Route tracking and map-based context |
| Power | Battery percentage, voltage, charging state | Device power and charging status |

If a sensor is warming up or temporarily unavailable, the device does not treat
that value as zero. The screen and app should show it as unavailable, commonly
as a blank value or dash depending on the interface.

## Operating Modes

AirGradient Go has three main operating modes. The mode controls which radios
and connectivity features are active.

| Mode | Main Use | Connectivity | Typical Behavior |
|---|---|---|---|
| Portable | Carrying the device with a phone | BLE | Streams live readings to the app and supports route history export |
| Stationary | Leaving the device in one place | Wi-Fi | Connects to Wi-Fi and can send readings to AirGradient cloud services |
| Offline / Airplane Mode | Use without radios | No BLE or Wi-Fi | Measures locally and can still track routes without transmitting data |

The default mode on a fresh device is Portable.

## Tracking And Route Logging

Tracking is one of the key Go features. When tracking is active, the device
combines air quality readings with GPS information and stores them as a route.

Tracking supports:

- Starting and stopping from the device menu.
- Starting and stopping from the connected phone app.
- GPS plus sensor data stored together as route points.
- A unique session ID for each tracking session.
- Continuing the same session across deep sleep wake-ups.
- Exporting stored route history to the phone over BLE.

The device also keeps a short temporary measurement history for on-device charts.
This chart data is separate from the persistent route log.

## Display And Navigation

The Go uses a 128 x 250 pixel monochrome e-paper display. E-paper is useful for a
portable device because it remains visible without constant display power.

The display can show:

- Current air quality readings.
- Battery and charging status.
- GPS, BLE, Wi-Fi, and tracking status icons.
- Menu screens and settings.
- Pairing passkeys.
- Setup and provisioning screens.
- Warning and shutdown messages.
- Small charts based on recent measurements.

User navigation is handled through three touch areas and physical buttons.
Common actions include opening the menu, changing settings, starting or stopping
tracking, confirming actions, and locking or unlocking the interface.

## Lock And Power Button Behavior

The device has a lock state so accidental touches do not change settings while
it is being carried.

- A short press on the power button locks or unlocks the interface.
- Touch navigation is ignored while locked.
- A long press powers the device off.
- If the user keeps holding the power button after shutdown on battery, the
  hardware can restart the device.
- Auto-lock can be configured or disabled.

## Phone App And BLE Features

In Portable mode, the Go acts as a BLE peripheral for the phone app.

Main BLE features:

- Device discovery as `AirGradient Go` with a short serial suffix.
- Secure pairing using a six-digit passkey shown on the e-paper display.
- Bonding, so future connections do not need the passkey again.
- Live sensor readings.
- GPS data while tracking or when GPS is enabled.
- Battery, charging, tracking, GPS, and storage status.
- Reading and changing device settings.
- Starting and stopping tracking.
- Clearing data.
- Factory reset.
- CO2 calibration command.
- GPS aiding from the phone to help GPS start faster.
- Downloading stored route history.
- BLE firmware update in Portable mode.

Only one phone can be connected at a time. When a phone is connected, the device
stops advertising until that phone disconnects.

## Wi-Fi And Cloud Features

In Stationary mode, the Go uses Wi-Fi instead of the Portable BLE data stream.
This mode is intended for use when the device is placed in one location.

Wi-Fi and cloud features include:

- Connecting with saved Wi-Fi credentials.
- Wi-Fi setup through BLE or a captive portal flow.
- Factory-default fallback credentials for setup scenarios.
- Saved network reconnect behavior after temporary outages.
- Optional cloud posting of latest readings.
- Static IP support when configured during provisioning.
- Wi-Fi firmware update checks in Stationary mode.

The device can also receive Wi-Fi credentials while still in Portable mode over
the already bonded BLE link. In that case the Wi-Fi radio is powered only when
needed for scan or verification, then turned off again to preserve battery.

## Offline Operation

Offline mode is for local use with no active BLE or Wi-Fi radio. This can be
useful for users who want radio silence, airplane-style operation, or local-only
tracking.

In Offline mode:

- The device continues measuring locally.
- Tracking can still combine sensor readings with GPS data.
- No BLE data stream is active.
- No Wi-Fi cloud posting is active.
- Firmware update is not available until the user changes to Portable or
  Stationary mode.

## Battery And Charging Features

AirGradient Go is battery-powered and includes battery management features for
portable use.

User-visible battery features:

- Battery percentage display.
- Charging status display.
- USB plugged-in indication.
- Low-battery warnings.
- Automatic protective shutdown when the battery is critically low.
- Over-temperature protection.
- Charging pause when the battery is full to reduce battery stress.

The firmware also manages sensor power to reduce drain. For example, the PM
sensor can be powered down between measurements when the measurement interval is
long enough.

## LEDs And Buzzer Feedback

AirGradient Go supports visual feedback through LEDs. On supported board
variants, it also supports a buzzer.

LED features:

- Front indicator brightness setting.
- Back AQI LED brightness setting.
- Touch feedback LED setting.
- AQI color display based on PM2.5.
- Boot and other short animations.

Buzzer and melody features:

- Buzzer on/off setting.
- Boot sound pattern.
- First-time welcome chime.
- Play Melody menu action.
- Built-in melody choices such as Chime and Tetris.
- Synchronized buzzer and LED effects for selected melodies.

Hardware note: LED and buzzer behavior depends on the board variant. Prototype
hardware may not include every indicator supported by V1 hardware.

## First-Time Setup

On a fresh device, the Go shows a first-time Getting Started flow after boot.
This flow includes a QR code that points users to the Go setup guide.

The Getting Started screen is shown until the user completes a first meaningful
engagement, such as:

- pressing `Start using`,
- successfully pairing with the phone app, or
- changing the operating mode.

Factory reset clears this state, so the setup guide can be shown again on a
reset or refurbished unit.

## Configurable Settings

The Go stores user settings on the device, so they survive power cycles.

Common configurable settings include:

- Measurement interval from 3 seconds up to 1 hour.
- Temperature unit: Celsius or Fahrenheit.
- PM display: micrograms per cubic meter or US AQI.
- GPS mode: always off, on when tracking, or always on.
- Operating mode: Portable, Stationary, or Offline.
- Auto-lock timeout.
- Front LED brightness.
- AQI LED brightness.
- Touch LED brightness.
- Buzzer enabled or disabled.
- Device name.
- Static IP and cloud preference for Stationary mode.

Action-style settings are also available from the device or app, including CO2
calibration, clearing stored data, and factory reset.

## Firmware Updates

Firmware update behavior depends on the current operating mode.

| Mode | Firmware Update Path |
|---|---|
| Portable | Phone-initiated update over BLE |
| Stationary | Device-initiated update check over Wi-Fi |
| Offline | No update path while Offline |

During a firmware update, normal sensing and data transfer are paused so the
update can complete safely.

## Data Storage And Export

The Go has two kinds of measurement storage:

- Temporary chart history for recent readings shown on the device.
- Persistent route history for tracking sessions.

Persistent route history survives deep sleep and power cycles. Users can export
route history through the phone app over BLE. Users can also clear stored data
from the device menu or the app.

## Product Notes For Team Members

- The Go supports both Prototype and V1 board variants with one firmware line.
- Some hardware features, especially LEDs, buzzer, and fuel gauge behavior,
  depend on board variant.
- CO2 sensor hardware can vary by build; the product behavior is still presented
  to users as CO2 measurement.
- Temperature and humidity may come from a dedicated sensor or from another
  sensor fallback depending on hardware availability.
- Missing readings should be described as unavailable, warming up, or not ready,
  not as zero.
- Portable BLE is the main app experience for carrying the device.
- Stationary Wi-Fi is the main experience for fixed-location use.
- Offline mode is local-only and radio-off.
