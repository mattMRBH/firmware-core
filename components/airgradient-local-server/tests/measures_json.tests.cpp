/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <cctype>
#include <cstring>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <cJSON.h>

#include "internal/measures_json.h"
#include "measures_types.h"
#include "types/system_info.h"

namespace {

SystemInfo make_info() {
  SystemInfo info;
  std::strncpy(info.serial_number, "aabbccddeeff", sizeof(info.serial_number) - 1);
  std::strncpy(info.model, "O-1PST", sizeof(info.model) - 1);
  std::strncpy(info.firmware, "2.0.0", sizeof(info.firmware) - 1);
  return info;
}

// Parse the serialized payload; caller must cJSON_Delete the result.
cJSON *serialize_and_parse(const Measures &m, const SystemInfo &info) {
  char buf[1024] = {};
  const size_t len = measures_json::serialize(m, info, buf, sizeof(buf));
  REQUIRE(len > 0);
  REQUIRE(len == std::strlen(buf));
  cJSON *root = cJSON_Parse(buf);
  REQUIRE(root != nullptr);
  return root;
}

std::string raw_number_str(const char *json, const char *key) {
  const std::string needle = std::string("\"") + key + "\":";
  const char *number = std::strstr(json, needle.c_str());
  if (number == nullptr) {
    return {};
  }
  number += needle.size();
  const char *end = number;
  while (*end && (std::isdigit(static_cast<unsigned char>(*end)) || *end == '.' || *end == '-' ||
                  *end == 'e' || *end == 'E' || *end == '+')) {
    ++end;
  }
  return std::string(number, end);
}

} // namespace

TEST_CASE("measures: identity always present, no measurement keys when invalid", "[measures]") {
  Measures m; // all invalid sentinels
  SystemInfo info = make_info();

  cJSON *root = serialize_and_parse(m, info);

  REQUIRE(cJSON_IsString(cJSON_GetObjectItem(root, "serialNumber")));
  REQUIRE(std::strcmp(cJSON_GetObjectItem(root, "serialNumber")->valuestring, "aabbccddeeff") == 0);
  REQUIRE(cJSON_IsString(cJSON_GetObjectItem(root, "model")));
  REQUIRE(cJSON_IsString(cJSON_GetObjectItem(root, "firmware")));
  // boot is always emitted (uptime in completed minutes).
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "boot")));

  // No wifiRssi when unavailable.
  REQUIRE(cJSON_GetObjectItem(root, "wifiRssi") == nullptr);

  // No measurement keys for invalid sentinels.
  REQUIRE(cJSON_GetObjectItem(root, "co2") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "pm01") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "temperature") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "humidity") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "tvocIndex") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "noxIndex") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "pm005Count") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "battPercent") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "battVolt") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "chargeVolt") == nullptr);

  cJSON_Delete(root);
}

TEST_CASE("measures: only valid fields are emitted", "[measures]") {
  Measures m;
  m.co2.co2 = 612;
  m.pm_a.pm_01 = 5.0f;
  m.pm_a.pm_25 = 8.0f;
  m.temp_hum_a.temperature = 24.3f;
  m.temp_hum_a.humidity = 47.1f;
  m.tvoc_nox.tvoc_index = 101;
  m.tvoc_nox.nox_index = 1;
  // pm_10 and the raw tvoc/nox stay invalid -> omitted.

  SystemInfo info = make_info();
  cJSON *root = serialize_and_parse(m, info);

  REQUIRE(cJSON_GetObjectItem(root, "co2")->valueint == 612);
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "pm01")));
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "pm25")));
  REQUIRE(cJSON_GetObjectItem(root, "pm10") == nullptr);
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "temperature")));
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "humidity")));
  REQUIRE(cJSON_GetObjectItem(root, "tvocIndex")->valueint == 101);
  REQUIRE(cJSON_GetObjectItem(root, "tvocRaw") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "noxIndex")->valueint == 1);
  REQUIRE(cJSON_GetObjectItem(root, "noxRaw") == nullptr);

  cJSON_Delete(root);
}

TEST_CASE("measures: wifiRssi emitted only when present", "[measures]") {
  Measures m;
  SystemInfo info = make_info();
  info.wifi_rssi = -57;

  cJSON *root = serialize_and_parse(m, info);
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "wifiRssi")));
  REQUIRE(cJSON_GetObjectItem(root, "wifiRssi")->valueint == -57);
  cJSON_Delete(root);
}

TEST_CASE("measures: boot retains the uint32 uptime wire range", "[measures]") {
  Measures m;
  SystemInfo info = make_info();
  info.boot = std::numeric_limits<uint32_t>::max();

  cJSON *root = serialize_and_parse(m, info);
  REQUIRE(cJSON_GetObjectItem(root, "boot")->valuedouble ==
          static_cast<double>(std::numeric_limits<uint32_t>::max()));
  cJSON_Delete(root);
}

TEST_CASE("measures: pm003Count maps from pm_03_pc", "[measures]") {
  Measures m;
  m.pm_a.pm_03_pc = 1234.0f;
  SystemInfo info = make_info();

  cJSON *root = serialize_and_parse(m, info);
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "pm003Count")));
  cJSON_Delete(root);
}

TEST_CASE("measures: all PM counts and power fields are emitted independently", "[measures]") {
  Measures m;
  m.pm_a.pm_03_pc = 100.4f;
  m.pm_a.pm_05_pc = 200.4f;
  m.pm_a.pm_01_pc = 300.6f;
  m.pm_a.pm_25_pc = 400.7f;
  m.pm_a.pm_5_pc = 500.8f;
  m.pm_a.pm_10_pc = 600.9f;
  m.power.battery_percentage = 52.0f;
  m.power.battery_voltage = 3.456f;
  m.power.charging_voltage = 5.678f;
  const SystemInfo info = make_info();

  cJSON *root = serialize_and_parse(m, info);
  REQUIRE(cJSON_GetObjectItem(root, "pm003Count")->valuedouble == 100.0);
  REQUIRE(cJSON_GetObjectItem(root, "pm005Count")->valuedouble == 200.0);
  REQUIRE(cJSON_GetObjectItem(root, "pm01Count")->valuedouble == 301.0);
  REQUIRE(cJSON_GetObjectItem(root, "pm02Count")->valuedouble == 401.0);
  REQUIRE(cJSON_GetObjectItem(root, "pm50Count")->valuedouble == 501.0);
  REQUIRE(cJSON_GetObjectItem(root, "pm10Count")->valuedouble == 601.0);
  REQUIRE(cJSON_GetObjectItem(root, "battPercent")->valuedouble == 52.0);
  REQUIRE(cJSON_GetObjectItem(root, "battVolt")->valuedouble == 3.46);
  REQUIRE(cJSON_GetObjectItem(root, "chargeVolt")->valuedouble == 5.68);
  cJSON_Delete(root);
}

TEST_CASE("measures: non-finite PM counts and voltages are omitted", "[measures]") {
  Measures m;
  m.pm_a.pm_03_pc = 123.0f;
  m.pm_a.pm_05_pc = std::numeric_limits<float>::infinity();
  m.power.battery_percentage = 42.0f;
  m.power.battery_voltage = std::numeric_limits<float>::infinity();
  m.power.charging_voltage = std::numeric_limits<float>::quiet_NaN();
  const SystemInfo info = make_info();

  cJSON *root = serialize_and_parse(m, info);
  REQUIRE(cJSON_GetObjectItem(root, "pm003Count") != nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "pm005Count") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "battPercent") != nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "battVolt") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "chargeVolt") == nullptr);
  cJSON_Delete(root);
}

TEST_CASE("measures: float fields use cloud payload precision", "[measures]") {
  Measures m;
  m.pm_a.pm_01 = 5.678f;
  m.pm_a.pm_25 = 8.123f;
  m.pm_a.pm_10 = 9.456f;
  m.pm_a.pm_03_pc = 1234.7f;
  m.temp_hum_a.temperature = 24.346f;
  m.temp_hum_a.humidity = 47.126f;
  const SystemInfo info = make_info();
  char buf[1024] = {};

  REQUIRE(measures_json::serialize(m, info, buf, sizeof(buf)) > 0);
  cJSON *root = cJSON_Parse(buf);
  REQUIRE(root != nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "pm01")->valuedouble == 5.7);
  REQUIRE(cJSON_GetObjectItem(root, "pm25")->valuedouble == 8.1);
  REQUIRE(cJSON_GetObjectItem(root, "pm10")->valuedouble == 9.5);
  REQUIRE(cJSON_GetObjectItem(root, "pm003Count")->valuedouble == 1235.0);
  REQUIRE(cJSON_GetObjectItem(root, "temperature")->valuedouble == 24.35);
  REQUIRE(cJSON_GetObjectItem(root, "humidity")->valuedouble == 47.13);
  cJSON_Delete(root);

  for (const char *key : {"pm01", "pm25", "pm10"}) {
    const std::string raw = raw_number_str(buf, key);
    REQUIRE_FALSE(raw.empty());
    const auto dot = raw.find('.');
    if (dot != std::string::npos) {
      REQUIRE((raw.size() - dot - 1) <= 1);
    }
  }
  for (const char *key : {"temperature", "humidity"}) {
    const std::string raw = raw_number_str(buf, key);
    REQUIRE_FALSE(raw.empty());
    const auto dot = raw.find('.');
    if (dot != std::string::npos) {
      REQUIRE((raw.size() - dot - 1) <= 2);
    }
  }
  REQUIRE(raw_number_str(buf, "pm003Count").find('.') == std::string::npos);
}

TEST_CASE("measures: tiny buffer fails cleanly", "[measures]") {
  Measures m;
  SystemInfo info = make_info();
  char tiny[8] = {};
  REQUIRE(measures_json::serialize(m, info, tiny, sizeof(tiny)) == 0);
}
