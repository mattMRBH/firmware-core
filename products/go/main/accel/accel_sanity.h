/**
 * AirGradient Go — Accelerometer sanity helpers
 *
 * Pure, hardware-free classification used by the Hardware Test accelerometer
 * flow: a WHO_AM_I identity check and an at-rest magnitude sanity check
 * (~1 g). Host-testable in isolation; depends on nothing but <cstdint>.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

/// Expected magnitude of the gravity vector at rest (milli-g).
inline constexpr int32_t ACCEL_REST_MG = 1000;
/// PASS tolerance band around ACCEL_REST_MG for the at-rest check (milli-g).
inline constexpr int32_t ACCEL_REST_TOL_MG = 150;

/// True when the identity byte matches the device's expected WHO_AM_I value.
inline constexpr bool accel_identity_ok(uint8_t who_am_i, uint8_t expected) {
  return who_am_i == expected;
}

/// True when the acceleration vector magnitude sits within ACCEL_REST_TOL_MG
/// of 1 g — the at-rest sanity check. Compares squared magnitudes to avoid a
/// sqrt; every term fits in int32 across the ±2 g range.
inline constexpr bool accel_rest_magnitude_ok(int16_t x_mg, int16_t y_mg, int16_t z_mg) {
  const int32_t x = x_mg;
  const int32_t y = y_mg;
  const int32_t z = z_mg;
  const int32_t magnitude_sq = x * x + y * y + z * z;
  const int32_t lo = ACCEL_REST_MG - ACCEL_REST_TOL_MG; // 850 mg
  const int32_t hi = ACCEL_REST_MG + ACCEL_REST_TOL_MG; // 1150 mg
  return magnitude_sq >= lo * lo && magnitude_sq <= hi * hi;
}
