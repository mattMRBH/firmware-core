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
  const auto res = parse(R"({"temperatureUnit":"f","co2AbcDays":7,"postDataToCloud":true})", cfg);

  REQUIRE(res.status == config_json::ParseStatus::Ok);
  REQUIRE(cfg.temperature_unit.has_value());
  REQUIRE(*cfg.temperature_unit == "f");
  REQUIRE(cfg.co2_abc_days.has_value());
  REQUIRE(*cfg.co2_abc_days == 7);
  REQUIRE(cfg.post_data_to_cloud.has_value());
  REQUIRE(*cfg.post_data_to_cloud == true);
  // Absent keys stay nullopt.
  REQUIRE_FALSE(cfg.country.has_value());
  REQUIRE_FALSE(cfg.led_mode.has_value());
}

TEST_CASE("config parse: all enum fields accept catalog values", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(
      R"({"pmStandard":"us-aqi","temperatureUnit":"c","configurationControl":"both","ledMode":"iaqs"})",
      cfg);
  REQUIRE(res.status == config_json::ParseStatus::Ok);
  REQUIRE(*cfg.pm_standard == "us-aqi");
  REQUIRE(*cfg.configuration_control == "both");
  REQUIRE(*cfg.led_mode == "iaqs");
}

TEST_CASE("config parse: cloudConnection and url fields parse", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res =
      parse(R"({"cloudConnection":false,"mqttBrokerUrl":"mqtt://x","httpDomain":""})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::Ok);
  REQUIRE(cfg.cloud_connection.has_value());
  REQUIRE(*cfg.cloud_connection == false);
  REQUIRE(*cfg.mqtt_broker_url == "mqtt://x");
  REQUIRE(*cfg.http_domain == "");
}

TEST_CASE("config parse: unknown key rejected with the offending key", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"temperatureUnits":"c"})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::UnknownField);
  REQUIRE(std::strcmp(res.unknown_key, "temperatureUnits") == 0);
}

TEST_CASE("config parse: wrong type rejected with field id", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"co2AbcDays":"seven"})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::Co2AbcDays);
}

TEST_CASE("config parse: bad enum rejected with field id", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"temperatureUnit":"k"})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::TemperatureUnit);
}

TEST_CASE("config parse: non-bool for bool field rejected", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"postDataToCloud":1})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::PostDataToCloud);
}

TEST_CASE("config parse: malformed JSON rejected", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(parse(R"({"temperatureUnit":)", cfg).status == config_json::ParseStatus::InvalidBody);
}

TEST_CASE("config parse: non-object root rejected", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(parse(R"([1,2,3])", cfg).status == config_json::ParseStatus::InvalidBody);
  REQUIRE(parse(R"("hello")", cfg).status == config_json::ParseStatus::InvalidBody);
  REQUIRE(parse(R"(42)", cfg).status == config_json::ParseStatus::InvalidBody);
}

TEST_CASE("config parse: trailing garbage rejected, trailing whitespace ok", "[config][parse]") {
  LocalServerConfig cfg;
  REQUIRE(parse(R"({"temperatureUnit":"c"} junk)", cfg).status ==
          config_json::ParseStatus::InvalidBody);
  REQUIRE(parse("{\"temperatureUnit\":\"c\"}   \n\t", cfg).status == config_json::ParseStatus::Ok);
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

TEST_CASE("config parse: corrections with populated pm25 and null slr", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(
      R"({"corrections":{"pm25":{"correctionAlgorithm":"slr_PMS5003_20231030","slr":{"intercept":0,"scalingFactor":0.02838,"useEpa2021":true}},"temp":{"correctionAlgorithm":"none","slr":null}}})",
      cfg);
  REQUIRE(res.status == config_json::ParseStatus::Ok);
  REQUIRE(cfg.corrections.has_value());
  REQUIRE(cfg.corrections->pm25.has_value());
  REQUIRE(cfg.corrections->pm25->algorithm == "slr_PMS5003_20231030");
  REQUIRE(cfg.corrections->pm25->slr.has_value());
  REQUIRE(cfg.corrections->pm25->slr->scaling_factor == 0.02838);
  REQUIRE(cfg.corrections->pm25->slr->use_epa2021.has_value());
  REQUIRE(*cfg.corrections->pm25->slr->use_epa2021 == true);
  REQUIRE(cfg.corrections->temp.has_value());
  REQUIRE(cfg.corrections->temp->algorithm == "none");
  REQUIRE_FALSE(cfg.corrections->temp->slr.has_value());
}

TEST_CASE("config parse: corrections unknown inner key rejected (dotted)", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res =
      parse(R"({"corrections":{"pm02":{"correctionAlgorithm":"none","slr":null}}})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::UnknownField);
  REQUIRE(std::strcmp(res.unknown_key, "corrections.pm02") == 0);
}

TEST_CASE("config parse: corrections unknown sub-key rejected (dotted)", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res =
      parse(R"({"corrections":{"temp":{"correctionAlgorithm":"none","bogus":1}}})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::UnknownField);
  REQUIRE(std::strcmp(res.unknown_key, "corrections.temp.bogus") == 0);
}

TEST_CASE("config parse: corrections non-object entry rejected with dotted field",
          "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"corrections":{"pm25":42}})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::CorrectionsPm25);
}

TEST_CASE("config parse: corrections non-object root rejected", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(R"({"corrections":[1,2]})", cfg);
  REQUIRE(res.status == config_json::ParseStatus::InvalidValue);
  REQUIRE(res.field == ConfigFieldId::Corrections);
}

TEST_CASE("config parse: useEpa2021 rejected outside pm25", "[config][parse]") {
  LocalServerConfig cfg;
  const auto res = parse(
      R"({"corrections":{"temp":{"correctionAlgorithm":"none","slr":{"intercept":0,"scalingFactor":1,"useEpa2021":true}}}})",
      cfg);
  REQUIRE(res.status == config_json::ParseStatus::UnknownField);
  REQUIRE(std::strcmp(res.unknown_key, "corrections.temp.slr.useEpa2021") == 0);
}

TEST_CASE("config serialize: emits only present fields", "[config][serialize]") {
  LocalServerConfig cfg;
  cfg.temperature_unit = "f";
  cfg.led_bar_brightness = 80;
  cfg.post_data_to_cloud = false;

  char buf[512] = {};
  const size_t len = config_json::serialize(cfg, buf, sizeof(buf));
  REQUIRE(len > 0);

  cJSON *root = cJSON_Parse(buf);
  REQUIRE(root != nullptr);
  REQUIRE(std::strcmp(cJSON_GetObjectItem(root, "temperatureUnit")->valuestring, "f") == 0);
  REQUIRE(cJSON_GetObjectItem(root, "ledBarBrightness")->valueint == 80);
  REQUIRE(cJSON_IsBool(cJSON_GetObjectItem(root, "postDataToCloud")));
  REQUIRE(cJSON_IsFalse(cJSON_GetObjectItem(root, "postDataToCloud")));
  // Absent fields omitted.
  REQUIRE(cJSON_GetObjectItem(root, "country") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "pmStandard") == nullptr);
  cJSON_Delete(root);
}

TEST_CASE("config serialize: corrections nest with slr and null slr", "[config][serialize]") {
  LocalServerConfig cfg;
  Corrections corr;
  CorrectionEntry pm25;
  pm25.algorithm = "slr_PMS5003_20231030";
  SlrParams slr;
  slr.intercept = 0.0;
  slr.scaling_factor = 0.02838;
  slr.use_epa2021 = true;
  pm25.slr = slr;
  corr.pm25 = pm25;
  CorrectionEntry temp;
  temp.algorithm = "none";
  corr.temp = temp; // slr stays nullopt -> "slr": null
  cfg.corrections = corr;

  char buf[1024] = {};
  const size_t len = config_json::serialize(cfg, buf, sizeof(buf));
  REQUIRE(len > 0);

  cJSON *root = cJSON_Parse(buf);
  REQUIRE(root != nullptr);
  cJSON *c = cJSON_GetObjectItem(root, "corrections");
  REQUIRE(cJSON_IsObject(c));
  cJSON *p = cJSON_GetObjectItem(c, "pm25");
  REQUIRE(std::strcmp(cJSON_GetObjectItem(p, "correctionAlgorithm")->valuestring,
                      "slr_PMS5003_20231030") == 0);
  cJSON *ps = cJSON_GetObjectItem(p, "slr");
  REQUIRE(cJSON_IsObject(ps));
  REQUIRE(cJSON_IsTrue(cJSON_GetObjectItem(ps, "useEpa2021")));
  cJSON *t = cJSON_GetObjectItem(c, "temp");
  REQUIRE(cJSON_IsNull(cJSON_GetObjectItem(t, "slr")));
  // useEpa2021 must not leak into non-pm25 entries.
  REQUIRE(cJSON_GetObjectItem(t, "useEpa2021") == nullptr);
  cJSON_Delete(root);
}

TEST_CASE("config field wire keys map correctly", "[config][parse]") {
  REQUIRE(std::strcmp(config_json::config_field_wire_key(ConfigFieldId::TemperatureUnit),
                      "temperatureUnit") == 0);
  REQUIRE(std::strcmp(config_json::config_field_wire_key(ConfigFieldId::DisplayBrightness),
                      "displayBrightness") == 0);
  REQUIRE(std::strcmp(config_json::config_field_wire_key(ConfigFieldId::CorrectionsPm25),
                      "corrections.pm25") == 0);
  REQUIRE(config_json::config_field_wire_key(ConfigFieldId::None) == nullptr);
}
