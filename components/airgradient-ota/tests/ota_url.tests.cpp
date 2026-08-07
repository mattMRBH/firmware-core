/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "services/ota_url.h"
#include "types/ota_types.h"

namespace {

OtaRequest make_request() { return OtaRequest{"aabbccddeeff", "3.1.21", "hw.airgradient.com"}; }

} // namespace

TEST_CASE("ota_url builds OneOpenAir URL with airgradient: prefix and generic/os path",
          "[ota_url]") {
  char url[256] = {0};
  const OtaRequest req = make_request();

  REQUIRE(ota_url::build(req, url, sizeof(url)));
  REQUIRE(std::string(url) ==
          "http://hw.airgradient.com/sensors/airgradient:aabbccddeeff/generic/os/"
          "firmware.bin?current_firmware=3.1.21");
}

TEST_CASE("ota_url rejects missing required fields", "[ota_url]") {
  char url[256] = {0};

  SECTION("null serial") {
    OtaRequest req = make_request();
    req.serial_number = nullptr;
    REQUIRE_FALSE(ota_url::build(req, url, sizeof(url)));
  }

  SECTION("empty serial") {
    OtaRequest req = make_request();
    req.serial_number = "";
    REQUIRE_FALSE(ota_url::build(req, url, sizeof(url)));
  }

  SECTION("null current_firmware") {
    OtaRequest req = make_request();
    req.current_firmware = nullptr;
    REQUIRE_FALSE(ota_url::build(req, url, sizeof(url)));
  }

  SECTION("null http_domain") {
    OtaRequest req = make_request();
    req.http_domain = nullptr;
    REQUIRE_FALSE(ota_url::build(req, url, sizeof(url)));
  }
}

TEST_CASE("ota_url fails on truncation", "[ota_url]") {
  char url[32] = {0};
  const OtaRequest req = make_request();

  REQUIRE_FALSE(ota_url::build(req, url, sizeof(url)));
}

TEST_CASE("ota_url rejects null/zero-size output buffer", "[ota_url]") {
  const OtaRequest req = make_request();
  char url[256] = {0};

  REQUIRE_FALSE(ota_url::build(req, nullptr, sizeof(url)));
  REQUIRE_FALSE(ota_url::build(req, url, 0));
}
