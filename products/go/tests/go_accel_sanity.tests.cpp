/**
 * AirGradient Go — Accelerometer sanity-helper unit tests
 *
 * Pure classification logic for the Hardware Test accelerometer flow:
 * WHO_AM_I identity check and the at-rest (~1 g) magnitude sanity check.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "accel/accel_sanity.h"

TEST_CASE("accel_identity_ok matches expected WHO_AM_I", "[accel]") {
  CHECK(accel_identity_ok(0x33, 0x33));
  CHECK_FALSE(accel_identity_ok(0x00, 0x33)); // absent / read failed
  CHECK_FALSE(accel_identity_ok(0x32, 0x33)); // wrong device
  CHECK_FALSE(accel_identity_ok(0xFF, 0x33));
}

TEST_CASE("accel_rest_magnitude_ok accepts ~1 g at rest", "[accel]") {
  SECTION("axis-aligned 1 g on each axis passes") {
    CHECK(accel_rest_magnitude_ok(1000, 0, 0));
    CHECK(accel_rest_magnitude_ok(0, -1000, 0));
    CHECK(accel_rest_magnitude_ok(0, 0, 1000));
  }

  SECTION("tilted but unit magnitude passes") {
    // ~577 mg on each axis → magnitude ≈ 1000 mg.
    CHECK(accel_rest_magnitude_ok(577, 577, 577));
    // 600/800/0 → magnitude exactly 1000 mg.
    CHECK(accel_rest_magnitude_ok(600, 800, 0));
  }

  SECTION("band edges") {
    CHECK(accel_rest_magnitude_ok(850, 0, 0));  // lower bound
    CHECK(accel_rest_magnitude_ok(1150, 0, 0)); // upper bound
  }
}

TEST_CASE("accel_rest_magnitude_ok rejects out-of-band magnitudes", "[accel]") {
  CHECK_FALSE(accel_rest_magnitude_ok(0, 0, 0));       // free fall / no gravity
  CHECK_FALSE(accel_rest_magnitude_ok(2000, 0, 0));    // 2 g (moving / mounted wrong)
  CHECK_FALSE(accel_rest_magnitude_ok(849, 0, 0));     // just below the band
  CHECK_FALSE(accel_rest_magnitude_ok(1151, 0, 0));    // just above the band
  CHECK_FALSE(accel_rest_magnitude_ok(1000, 1000, 0)); // ~1414 mg
}

TEST_CASE("accel sanity constants define a ±150 mg band around 1 g", "[accel]") {
  CHECK(ACCEL_REST_MG == 1000);
  CHECK(ACCEL_REST_TOL_MG == 150);
}
