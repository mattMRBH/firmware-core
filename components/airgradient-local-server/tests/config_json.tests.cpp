/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <cstring>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <cJSON.h>

#include "internal/config_json.h"
#include "types/local_config.h"

namespace {

config_json::ParseResult parse(const std::string &body, LocalServerConfig &out) {
  return config_json::parse(body.data(), body.size(), out);
}

} // namespace

TEST_CASE("config parse: valid partial body sets only present keys", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"temp_unit":"f","co2_calib_days":7,"cloud_enabled":true})", cfg);

  REQUIRE(res.status == config_json::ParseStatus::Ok);
  REQUIRE(cfg.temp_unit.has_value());
  REQUIRE(*cfg.temp_unit == "f");
  REQUIRE(cfg.co2_calib_days.has_value());
  REQUIRE(*cfg.co2_calib_days == 7);
  REQUIRE(cfg.cloud_enabled.has_value());
  REQUIRE(*cfg.cloud_enabled == true);
  // Absent keys stay nullopt.
  REQUIRE_FALSE(cfg.country.has_value());
  REQUIRE_FALSE(cfg.led_bar_mode.has_value());
}

TEST_CASE("config parse: all enum fields accept catalog values", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(
      R"({"pm_standard":"us-aqi","temp_unit":"c","configuration_control":"both","led_bar_mode":"co2"})",
      cfg);
  REQUIRE(res.status == config_json::ParseStatus::Ok);
  REQUIRE(*cfg.pm_standard == "us-aqi");
  REQUIRE(*cfg.configuration_control == "both");
  REQUIRE(*cfg.led_bar_mode == "co2");
}

TEST_CASE("config parse: unknown key rejected with the offending key", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"temp_units":"c"})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::UnknownField);
  REQUIRE(std::strcmp(res.unknown_key, "temp_units") == 0);
}

TEST_CASE("config parse: wrong type rejected with field id", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"co2_calib_days":"seven"})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::Co2CalibDays);
}

TEST_CASE("config parse: bad enum rejected with field id", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"temp_unit":"k"})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::TempUnit);
}

TEST_CASE("config parse: non-bool for bool field rejected", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"cloud_enabled":1})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::CloudEnabled);
}

TEST_CASE("config parse: malformed JSON rejected", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(parse(R"({"temp_unit":)", cfg).status == config_json::ParseStatus::InvalidBody);
}

TEST_CASE("config parse: non-object root rejected", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(parse(R"([1,2,3])", cfg).status == config_json::ParseStatus::InvalidBody);
  REQUIRE(parse(R"("hello")", cfg).status == config_json::ParseStatus::InvalidBody);
  REQUIRE(parse(R"(42)", cfg).status == config_json::ParseStatus::InvalidBody);
}

TEST_CASE("config parse: trailing garbage rejected, trailing whitespace ok", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(parse(R"({"temp_unit":"c"} junk)", cfg).status == config_json::ParseStatus::InvalidBody);
  REQUIRE(parse("{\"temp_unit\":\"c\"}   \n\t", cfg).status == config_json::ParseStatus::Ok);
}

TEST_CASE("config parse: empty body rejected", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(config_json::parse(nullptr, 0, cfg).status == config_json::ParseStatus::InvalidBody);
  REQUIRE(parse("", cfg).status == config_json::ParseStatus::InvalidBody);
}

TEST_CASE("config parse: empty object is a valid no-op", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(parse("{}", cfg).status == config_json::ParseStatus::Ok);
}

TEST_CASE("config serialize: emits only present fields", "[config][serialize]") {
  LocalServerConfig cfg;
  cfg.temp_unit = "f";
  cfg.led_bar_brightness = 80;
  cfg.cloud_enabled = false;

  char buf[512] = {};
  const size_t len = config_json::serialize(cfg, buf, sizeof(buf));
  REQUIRE(len > 0);

  cJSON *root = cJSON_Parse(buf);
  REQUIRE(root != nullptr);
  REQUIRE(std::strcmp(cJSON_GetObjectItem(root, "temp_unit")->valuestring, "f") == 0);
  REQUIRE(cJSON_GetObjectItem(root, "led_bar_brightness")->valueint == 80);
  REQUIRE(cJSON_IsBool(cJSON_GetObjectItem(root, "cloud_enabled")));
  REQUIRE(cJSON_IsFalse(cJSON_GetObjectItem(root, "cloud_enabled")));
  // Absent fields omitted.
  REQUIRE(cJSON_GetObjectItem(root, "country") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "pm_standard") == nullptr);
  cJSON_Delete(root);
}

TEST_CASE("config field wire keys map correctly", "[config][parse]") {
  REQUIRE(std::strcmp(config_json::config_field_wire_key(ConfigFieldId::TempUnit), "temp_unit") ==
          0);
  REQUIRE(std::strcmp(config_json::config_field_wire_key(ConfigFieldId::DisplayBrightness),
                      "display_brightness") == 0);
  REQUIRE(config_json::config_field_wire_key(ConfigFieldId::None) == nullptr);
}
