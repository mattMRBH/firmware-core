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
// BmsStatus
// ---------------------------------------------------------------------------

/// Charging-state status snapshot from a BMS device.
struct BmsStatus {
  BmsChargingState charging_state = BmsChargingState::Unknown;

  bool is_charging_state_valid() const { return charging_state != BmsChargingState::Unknown; }

  bool is_valid() const { return is_charging_state_valid(); }
};

#endif // BMS_TYPES_H
