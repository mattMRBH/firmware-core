/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cJSON.h"

#include "services/payload_serializer.h"
#include "types/client_types.h"

namespace {

// All fields invalid; tests set only what they exercise.
Measures make_invalid_measures() {
  Measures m{};
  m.co2.co2 = MeasuresInvalid::CO2;
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.temp_hum_b.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_b.humidity = MeasuresInvalid::HUMIDITY;
  m.pm_a.pm_01 = MeasuresInvalid::PM;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.pm_a.pm_10 = MeasuresInvalid::PM;
  m.pm_a.pm_01_sp = MeasuresInvalid::PM;
  m.pm_a.pm_25_sp = MeasuresInvalid::PM;
  m.pm_a.pm_10_sp = MeasuresInvalid::PM;
  m.pm_a.pm_03_pc = MeasuresInvalid::PM;
  m.pm_a.pm_05_pc = MeasuresInvalid::PM;
  m.pm_a.pm_01_pc = MeasuresInvalid::PM;
  m.pm_a.pm_25_pc = MeasuresInvalid::PM;
  m.pm_a.pm_5_pc = MeasuresInvalid::PM;
  m.pm_a.pm_10_pc = MeasuresInvalid::PM;
  m.pm_b.pm_01 = MeasuresInvalid::PM;
  m.pm_b.pm_25 = MeasuresInvalid::PM;
  m.pm_b.pm_10 = MeasuresInvalid::PM;
  m.pm_b.pm_03_pc = MeasuresInvalid::PM;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  m.electrode.o3_we = MeasuresInvalid::VOLT;
  m.electrode.o3_ae = MeasuresInvalid::VOLT;
  m.electrode.no2_we = MeasuresInvalid::VOLT;
  m.electrode.no2_ae = MeasuresInvalid::VOLT;
  m.electrode.afe_temp = MeasuresInvalid::VOLT;
  // MeasuresPower already defaults to invalid sentinels.
  return m;
}

// Full-Measures view.
MeasuresInput input_from_full(const Measures &m) {
  MeasuresInput in;
  in.temp_hum_a = &m.temp_hum_a;
  in.temp_hum_b = &m.temp_hum_b;
  in.pm_a = &m.pm_a;
  in.pm_b = &m.pm_b;
  in.co2 = &m.co2;
  in.tvoc_nox = &m.tvoc_nox;
  in.power = &m.power;
  in.electrode = &m.electrode;
  return in;
}

// MeasuresBasic-shaped view.
MeasuresInput input_basic_view(const Measures &m) {
  MeasuresInput in;
  in.temp_hum_a = &m.temp_hum_a;
  in.pm_a = &m.pm_a;
  in.co2 = &m.co2;
  in.tvoc_nox = &m.tvoc_nox;
  return in;
}

struct ParsedJson {
  cJSON *doc;
  explicit ParsedJson(const char *s) : doc(cJSON_Parse(s)) {}
  ~ParsedJson() { cJSON_Delete(doc); }
  bool has(const char *key) const { return cJSON_GetObjectItem(doc, key) != nullptr; }
  double number(const char *key) const { return cJSON_GetObjectItem(doc, key)->valuedouble; }
};

} // namespace

TEST_CASE("serializer always includes signal", "[payload_serializer]") {
  const auto m = make_invalid_measures();
  const auto in = input_from_full(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, -55, buf, sizeof(buf), &written));
  REQUIRE(written > 0);
  ParsedJson p(buf);
  REQUIRE(p.doc != nullptr);
  REQUIRE(p.has("wifi"));
  REQUIRE(p.number("wifi") == -55);
}

TEST_CASE("serializer omits invalid fields", "[payload_serializer]") {
  const auto m = make_invalid_measures();
  const auto in = input_from_full(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, -55, buf, sizeof(buf), &written));
  ParsedJson p(buf);
  REQUIRE_FALSE(p.has("rco2"));
  REQUIRE_FALSE(p.has("atmp"));
  REQUIRE_FALSE(p.has("rhum"));
  REQUIRE_FALSE(p.has("pm02"));
  REQUIRE_FALSE(p.has("tvocIndex"));
  REQUIRE_FALSE(p.has("volt"));
}

TEST_CASE("serializer includes valid fields", "[payload_serializer]") {
  auto m = make_invalid_measures();
  m.co2.co2 = 450;
  m.temp_hum_a.temperature = 23.5f;
  m.temp_hum_a.humidity = 42.0f;
  m.tvoc_nox.tvoc_index = 100;

  const auto in = input_from_full(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, -55, buf, sizeof(buf), &written));
  ParsedJson p(buf);
  REQUIRE(p.has("rco2"));
  REQUIRE(p.number("rco2") == 450);
  REQUIRE(p.has("atmp"));
  REQUIRE_THAT(p.number("atmp"), Catch::Matchers::WithinAbs(23.5, 0.001));
  REQUIRE(p.has("rhum"));
  REQUIRE_THAT(p.number("rhum"), Catch::Matchers::WithinAbs(42.0, 0.001));
  REQUIRE(p.has("tvocIndex"));
  REQUIRE(p.number("tvocIndex") == 100);
}

TEST_CASE("dual-channel PM averaging when both valid", "[payload_serializer]") {
  auto m = make_invalid_measures();
  m.pm_a.pm_25 = 10.0f;
  m.pm_b.pm_25 = 20.0f;

  const auto in = input_from_full(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, 0, buf, sizeof(buf), &written));
  ParsedJson p(buf);
  REQUIRE(p.has("pm02"));
  REQUIRE_THAT(p.number("pm02"), Catch::Matchers::WithinAbs(15.0, 0.001));
}

TEST_CASE("dual-channel PM uses valid channel when only one valid", "[payload_serializer]") {
  auto m = make_invalid_measures();
  m.pm_a.pm_25 = 10.0f;
  // pm_b.pm_25 stays invalid

  const auto in = input_from_full(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, 0, buf, sizeof(buf), &written));
  ParsedJson p(buf);
  REQUIRE(p.has("pm02"));
  REQUIRE_THAT(p.number("pm02"), Catch::Matchers::WithinAbs(10.0, 0.001));
}

TEST_CASE("dual-channel temperature averaging", "[payload_serializer]") {
  auto m = make_invalid_measures();
  m.temp_hum_a.temperature = 20.0f;
  m.temp_hum_b.temperature = 22.0f;

  const auto in = input_from_full(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, 0, buf, sizeof(buf), &written));
  ParsedJson p(buf);
  REQUIRE(p.has("atmp"));
  REQUIRE_THAT(p.number("atmp"), Catch::Matchers::WithinAbs(21.0, 0.001));
}

TEST_CASE("electrode fields serialised when valid", "[payload_serializer]") {
  auto m = make_invalid_measures();
  m.electrode.o3_we = 0.5f;
  m.electrode.no2_ae = 0.25f;

  const auto in = input_from_full(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, 0, buf, sizeof(buf), &written));
  ParsedJson p(buf);
  REQUIRE(p.has("measure0"));
  REQUIRE(p.has("measure3"));
  REQUIRE_FALSE(p.has("measure1"));
}

TEST_CASE("basic-variant view omits dual channel and electrode fields", "[payload_serializer]") {
  auto m = make_invalid_measures();
  // Valid data here should still be omitted -- the Basic view skips them.
  m.pm_b.pm_25 = 99.0f;
  m.electrode.o3_we = 1.0f;
  m.power.battery_voltage = 4.1f;
  m.temp_hum_a.temperature = 21.0f;
  m.co2.co2 = 500;

  const auto in = input_basic_view(m);
  char buf[512];
  size_t written = 0;
  REQUIRE(serialize_measures_json(in, -50, buf, sizeof(buf), &written));
  ParsedJson p(buf);
  REQUIRE(p.has("wifi"));
  REQUIRE(p.has("atmp"));
  REQUIRE(p.has("rco2"));
  REQUIRE_FALSE(p.has("measure0")); // electrode -- not in view
  REQUIRE_FALSE(p.has("volt"));     // power not in view
  REQUIRE_FALSE(p.has("pm02"));     // pm_b not in view, pm_a invalid
}

TEST_CASE("serializer returns false when buffer too small", "[payload_serializer]") {
  auto m = make_invalid_measures();
  m.co2.co2 = 450;
  const auto in = input_from_full(m);
  char tiny[4];
  size_t written = 0;
  REQUIRE_FALSE(serialize_measures_json(in, -55, tiny, sizeof(tiny), &written));
  REQUIRE(written == 0);
}
