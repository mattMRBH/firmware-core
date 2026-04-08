/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BMS_TYPES_H
#define BMS_TYPES_H

#include <cstdint>

// ---------------------------------------------------------------------------
// Invalid sentinels
// ---------------------------------------------------------------------------

namespace BmsInvalid {
static constexpr float VOLT = -1.0f;
} // namespace BmsInvalid

// ---------------------------------------------------------------------------
// Validation range
// ---------------------------------------------------------------------------

namespace BmsRange {
static constexpr float MIN_VALID_VOLT = 0.0f;
} // namespace BmsRange

// ---------------------------------------------------------------------------
// BmsChargingState
// ---------------------------------------------------------------------------

/// Shared public charging-state enum.  Replaces the driver-local
/// BQ25XX::ChargingStatus so that higher-level code does not need to
/// depend on a specific charger driver header.
enum class BmsChargingState : uint8_t {
  Unknown,
  NotCharging,
  TrickleCharge,
  PreCharge,
  FastCharge,
  TaperCharge,
  TopOffTimerActiveCharging,
  ChargeTerminationDone,
};

/// Human-readable label for a BmsChargingState value.
inline const char *bms_charging_state_str(BmsChargingState s) {
  switch (s) {
  case BmsChargingState::Unknown:
    return "Unknown";
  case BmsChargingState::NotCharging:
    return "NotCharging";
  case BmsChargingState::TrickleCharge:
    return "TrickleCharge";
  case BmsChargingState::PreCharge:
    return "PreCharge";
  case BmsChargingState::FastCharge:
    return "FastCharge";
  case BmsChargingState::TaperCharge:
    return "TaperCharge";
  case BmsChargingState::TopOffTimerActiveCharging:
    return "TopOff";
  case BmsChargingState::ChargeTerminationDone:
    return "Done";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// BmsTelemetry
// ---------------------------------------------------------------------------

/// Voltage telemetry snapshot from a BMS device.
/// Replaces the old BatteryMgmtData struct that lived in measures_types.h.
struct BmsTelemetry {
  float battery_voltage = BmsInvalid::VOLT;
  float charging_voltage = BmsInvalid::VOLT;

  bool is_battery_voltage_valid() const { return battery_voltage >= BmsRange::MIN_VALID_VOLT; }

  bool is_charging_voltage_valid() const { return charging_voltage >= BmsRange::MIN_VALID_VOLT; }

  bool is_valid() const { return is_battery_voltage_valid() && is_charging_voltage_valid(); }
};

// ---------------------------------------------------------------------------
// BmsPowerSource
// ---------------------------------------------------------------------------

/// Generic power source / adapter type detected by the charger IC.
/// Maps from device-specific VBUS status registers.
enum class BmsPowerSource : uint8_t {
  Unknown,        ///< Status not read or not supported
  None,           ///< No adapter connected
  UsbSdp,         ///< USB Standard Downstream Port (500 mA)
  UsbCdp,         ///< USB Charging Downstream Port (1.5 A)
  UsbDcp,         ///< USB Dedicated Charging Port (1.5 A)
  UnknownAdapter, ///< Adapter detected, type unknown (500 mA)
  NonStandard,    ///< Non-standard adapter (1 A / 2.1 A / 2.4 A)
  OtgMode,        ///< OTG boost mode active (device is source)
};

/// Human-readable label for a BmsPowerSource value.
inline const char *bms_power_source_str(BmsPowerSource s) {
  switch (s) {
  case BmsPowerSource::Unknown:
    return "Unknown";
  case BmsPowerSource::None:
    return "None";
  case BmsPowerSource::UsbSdp:
    return "USB_SDP";
  case BmsPowerSource::UsbCdp:
    return "USB_CDP";
  case BmsPowerSource::UsbDcp:
    return "USB_DCP";
  case BmsPowerSource::UnknownAdapter:
    return "UnknownAdapter";
  case BmsPowerSource::NonStandard:
    return "NonStandard";
  case BmsPowerSource::OtgMode:
    return "OTG";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// BmsStatus
// ---------------------------------------------------------------------------

/// Charger status snapshot from a BMS device.
///
/// Contains charging state, power source identification, and charger
/// regulation / fault flags.  All boolean flags default to false and
/// enum fields default to Unknown so that implementations that only
/// populate a subset still produce a well-defined result.
struct BmsStatus {
  BmsChargingState charging_state = BmsChargingState::Unknown;
  BmsPowerSource power_source = BmsPowerSource::Unknown;

  bool thermal_regulation = false;       ///< Charger in thermal regulation
  bool vsys_regulation = false;          ///< VSYS at minimum system voltage
  bool input_current_regulation = false; ///< Input current limit active
  bool input_voltage_regulation = false; ///< Input voltage limit active
  bool safety_timer_expired = false;     ///< Safety timer has expired
  bool watchdog_expired = false;         ///< Watchdog timer has expired

  bool is_charging_state_valid() const { return charging_state != BmsChargingState::Unknown; }

  bool is_power_source_valid() const { return power_source != BmsPowerSource::Unknown; }

  bool is_valid() const { return is_charging_state_valid(); }
};

#endif // BMS_TYPES_H
