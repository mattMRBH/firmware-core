/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <cstring>

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

} // namespace

TEST_CASE("measures: identity always present, no measurement keys when invalid", "[measures]") {
  Measures m; // all invalid sentinels
  SystemInfo info = make_info();

  cJSON *root = serialize_and_parse(m, info);

  REQUIRE(cJSON_IsString(cJSON_GetObjectItem(root, "serialNumber")));
  REQUIRE(std::strcmp(cJSON_GetObjectItem(root, "serialNumber")->valuestring, "aabbccddeeff") == 0);
  REQUIRE(cJSON_IsString(cJSON_GetObjectItem(root, "model")));
  REQUIRE(cJSON_IsString(cJSON_GetObjectItem(root, "firmware")));
  // boot is always emitted (measurement-cycle counter).
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "boot")));

  // No wifiRssi when unavailable.
  REQUIRE(cJSON_GetObjectItem(root, "wifiRssi") == nullptr);

  // No measurement keys for invalid sentinels.
  REQUIRE(cJSON_GetObjectItem(root, "co2") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "pm01") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "temp") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "humidity") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "tvocIndex") == nullptr);
  REQUIRE(cJSON_GetObjectItem(root, "noxIndex") == nullptr);

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
  REQUIRE(cJSON_IsNumber(cJSON_GetObjectItem(root, "temp")));
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

TEST_CASE("measures: boot reflects the cycle counter", "[measures]") {
  Measures m;
  SystemInfo info = make_info();
  info.boot = 6;

  cJSON *root = serialize_and_parse(m, info);
  REQUIRE(cJSON_GetObjectItem(root, "boot")->valueint == 6);
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

TEST_CASE("measures: tiny buffer fails cleanly", "[measures]") {
  Measures m;
  SystemInfo info = make_info();
  char tiny[8] = {};
  REQUIRE(measures_json::serialize(m, info, tiny, sizeof(tiny)) == 0);
}
