/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "types/bms_types.h"

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// BmsInvalid sentinel constants
// ---------------------------------------------------------------------------

TEST_CASE("BmsInvalid sentinel values", "[BmsTypes]") { REQUIRE(BmsInvalid::VOLT < 0.0f); }

// ---------------------------------------------------------------------------
// BmsTelemetry validation
// ---------------------------------------------------------------------------

TEST_CASE("BmsTelemetry default-constructed is invalid", "[BmsTypes]") {
  BmsTelemetry t{};
  REQUIRE_FALSE(t.is_battery_voltage_valid());
  REQUIRE_FALSE(t.is_charging_voltage_valid());
  REQUIRE_FALSE(t.is_valid());
}

TEST_CASE("BmsTelemetry with valid voltages", "[BmsTypes]") {
  BmsTelemetry t{};
  t.battery_voltage = 3.7f;
  t.charging_voltage = 5.0f;

  REQUIRE(t.is_battery_voltage_valid());
  REQUIRE(t.is_charging_voltage_valid());
  REQUIRE(t.is_valid());
}

TEST_CASE("BmsTelemetry with zero voltage is valid", "[BmsTypes]") {
  BmsTelemetry t{};
  t.battery_voltage = 0.0f;
  t.charging_voltage = 0.0f;

  REQUIRE(t.is_battery_voltage_valid());
  REQUIRE(t.is_charging_voltage_valid());
  REQUIRE(t.is_valid());
}

TEST_CASE("BmsTelemetry partial validity", "[BmsTypes]") {
  SECTION("Battery valid, charging invalid") {
    BmsTelemetry t{};
    t.battery_voltage = 3.7f;
    // charging_voltage left at default sentinel

    REQUIRE(t.is_battery_voltage_valid());
    REQUIRE_FALSE(t.is_charging_voltage_valid());
    REQUIRE_FALSE(t.is_valid());
  }

  SECTION("Battery invalid, charging valid") {
    BmsTelemetry t{};
    // battery_voltage left at default sentinel
    t.charging_voltage = 5.0f;

    REQUIRE_FALSE(t.is_battery_voltage_valid());
    REQUIRE(t.is_charging_voltage_valid());
    REQUIRE_FALSE(t.is_valid());
  }
}

// ---------------------------------------------------------------------------
// BmsStatus validation
// ---------------------------------------------------------------------------

TEST_CASE("BmsStatus default-constructed is invalid", "[BmsTypes]") {
  BmsStatus s{};
  REQUIRE(s.charging_state == BmsChargingState::Unknown);
  REQUIRE_FALSE(s.is_charging_state_valid());
  REQUIRE_FALSE(s.is_valid());
}

TEST_CASE("BmsStatus with known charging state is valid", "[BmsTypes]") {
  BmsStatus s{};
  s.charging_state = BmsChargingState::FastCharge;

  REQUIRE(s.is_charging_state_valid());
  REQUIRE(s.is_valid());
}

TEST_CASE("BmsStatus all charging states", "[BmsTypes]") {
  // Unknown should be invalid; all others should be valid
  REQUIRE_FALSE(BmsStatus{BmsChargingState::Unknown}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::NotCharging}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::TrickleCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::PreCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::FastCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::TaperCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::TopOffTimerActiveCharging}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::ChargeTerminationDone}.is_charging_state_valid());
}

// ---------------------------------------------------------------------------
// BmsChargingState enum values
// ---------------------------------------------------------------------------

TEST_CASE("BmsChargingState enum distinct values", "[BmsTypes]") {
  // Verify all enum values are distinct
  REQUIRE(static_cast<uint8_t>(BmsChargingState::Unknown) !=
          static_cast<uint8_t>(BmsChargingState::NotCharging));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::NotCharging) !=
          static_cast<uint8_t>(BmsChargingState::TrickleCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::TrickleCharge) !=
          static_cast<uint8_t>(BmsChargingState::PreCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::PreCharge) !=
          static_cast<uint8_t>(BmsChargingState::FastCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::FastCharge) !=
          static_cast<uint8_t>(BmsChargingState::TaperCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::TaperCharge) !=
          static_cast<uint8_t>(BmsChargingState::TopOffTimerActiveCharging));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::TopOffTimerActiveCharging) !=
          static_cast<uint8_t>(BmsChargingState::ChargeTerminationDone));
}
