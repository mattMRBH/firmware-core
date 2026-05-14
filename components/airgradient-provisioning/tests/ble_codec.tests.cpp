/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include <cJSON.h>

#include "internal/ble_codec.h"

namespace {

// Parse a JSON string from a byte buffer for assertion helpers.
cJSON *parse_buf(const uint8_t *buf, size_t len) {
  return cJSON_ParseWithLength(reinterpret_cast<const char *>(buf), len);
}

std::string get_string(cJSON *root, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return item->valuestring;
  }
  return {};
}

int get_int(cJSON *root, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsNumber(item)) {
    return static_cast<int>(item->valuedouble);
  }
  return -9999;
}

} // namespace

// ============================================================================
// parse_credentials
// ============================================================================

TEST_CASE("BleCodec: parse valid credentials with all fields", "[ble_codec]") {
  const char *json = R"({"ssid":"HomeWiFi","password":"secret","disableCloud":true})";
  ProvisioningData data;
  REQUIRE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json), std::strlen(json),
                                      data));
  REQUIRE(std::string(data.ssid) == "HomeWiFi");
  REQUIRE(std::string(data.password) == "secret");
  REQUIRE(data.disable_cloud == true);
}

TEST_CASE("BleCodec: parse credentials with ssid only (backward compat)", "[ble_codec]") {
  const char *json = R"({"ssid":"OpenNet"})";
  ProvisioningData data;
  REQUIRE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json), std::strlen(json),
                                      data));
  REQUIRE(std::string(data.ssid) == "OpenNet");
  REQUIRE(std::string(data.password) == "");
  REQUIRE(data.disable_cloud == false);
}

TEST_CASE("BleCodec: parse credentials with ssid and password, no disableCloud", "[ble_codec]") {
  const char *json = R"({"ssid":"Net","password":"pass123"})";
  ProvisioningData data;
  REQUIRE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json), std::strlen(json),
                                      data));
  REQUIRE(std::string(data.ssid) == "Net");
  REQUIRE(std::string(data.password) == "pass123");
  REQUIRE(data.disable_cloud == false);
}

TEST_CASE("BleCodec: parse credentials rejects missing ssid", "[ble_codec]") {
  const char *json = R"({"password":"secret"})";
  ProvisioningData data;
  REQUIRE_FALSE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json),
                                            std::strlen(json), data));
}

TEST_CASE("BleCodec: parse credentials rejects empty ssid", "[ble_codec]") {
  const char *json = R"({"ssid":"","password":"secret"})";
  ProvisioningData data;
  REQUIRE_FALSE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json),
                                            std::strlen(json), data));
}

TEST_CASE("BleCodec: parse credentials with staticIp", "[ble_codec]") {
  const char *json =
      R"({"ssid":"HomeWiFi","password":"secret","staticIp":{"ip":"192.168.110.251","netmask":"255.255.255.0","gateway":"192.168.110.1","dns":"8.8.8.8"}})";
  ProvisioningData data;
  REQUIRE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json), std::strlen(json),
                                      data));
  REQUIRE(std::string(data.ssid) == "HomeWiFi");
  REQUIRE(std::string(data.password) == "secret");
  REQUIRE(data.has_static_ip());
  // 192.168.110.251 in network byte order
  REQUIRE(data.static_ip.ip == (192u | (168u << 8) | (110u << 16) | (251u << 24)));
  REQUIRE(data.static_ip.netmask == (255u | (255u << 8) | (255u << 16) | (0u << 24)));
  REQUIRE(data.static_ip.gateway == (192u | (168u << 8) | (110u << 16) | (1u << 24)));
  REQUIRE(data.static_ip.dns_primary == (8u | (8u << 8) | (8u << 16) | (8u << 24)));
}

TEST_CASE("BleCodec: parse credentials without staticIp defaults to DHCP", "[ble_codec]") {
  const char *json = R"({"ssid":"HomeWiFi","password":"secret"})";
  ProvisioningData data;
  REQUIRE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json), std::strlen(json),
                                      data));
  REQUIRE_FALSE(data.has_static_ip());
  REQUIRE(data.static_ip.ip == 0);
}

TEST_CASE("BleCodec: parse credentials rejects malformed JSON", "[ble_codec]") {
  const char *json = "not json at all";
  ProvisioningData data;
  REQUIRE_FALSE(BleCodec::parse_credentials(reinterpret_cast<const uint8_t *>(json),
                                            std::strlen(json), data));
}

TEST_CASE("BleCodec: parse credentials rejects null/empty input", "[ble_codec]") {
  ProvisioningData data;
  REQUIRE_FALSE(BleCodec::parse_credentials(nullptr, 0, data));
  REQUIRE_FALSE(BleCodec::parse_credentials(nullptr, 10, data));
  uint8_t dummy = 0;
  REQUIRE_FALSE(BleCodec::parse_credentials(&dummy, 0, data));
}

// ============================================================================
// encode_scan_page
// ============================================================================

TEST_CASE("BleCodec: encode scan page with networks", "[ble_codec]") {
  WifiScanEntry entries[2] = {};
  std::strncpy(entries[0].ssid, "HomeWiFi", sizeof(entries[0].ssid) - 1);
  entries[0].rssi = -45;
  entries[0].auth_mode = WifiAuthMode::wpa2_psk;

  std::strncpy(entries[1].ssid, "Guest", sizeof(entries[1].ssid) - 1);
  entries[1].rssi = -62;
  entries[1].auth_mode = WifiAuthMode::open;

  uint8_t buf[512];
  size_t len = BleCodec::encode_scan_page(entries, 2, 1, 4, 10, buf, sizeof(buf));
  REQUIRE(len > 0);

  cJSON *root = parse_buf(buf, len);
  REQUIRE(root != nullptr);

  REQUIRE(get_int(root, "page") == 1);
  REQUIRE(get_int(root, "tpage") == 4);
  REQUIRE(get_int(root, "found") == 10);

  cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
  REQUIRE(cJSON_IsArray(wifi));
  REQUIRE(cJSON_GetArraySize(wifi) == 2);

  cJSON *first = cJSON_GetArrayItem(wifi, 0);
  REQUIRE(get_string(first, "s") == "HomeWiFi");
  REQUIRE(get_int(first, "r") == -45);
  REQUIRE(get_int(first, "o") == 0); // secured

  cJSON *second = cJSON_GetArrayItem(wifi, 1);
  REQUIRE(get_string(second, "s") == "Guest");
  REQUIRE(get_int(second, "r") == -62);
  REQUIRE(get_int(second, "o") == 1); // open

  cJSON_Delete(root);
}

// ============================================================================
// encode_scan_empty
// ============================================================================

TEST_CASE("BleCodec: encode empty scan result", "[ble_codec]") {
  uint8_t buf[64];
  size_t len = BleCodec::encode_scan_empty(buf, sizeof(buf));
  REQUIRE(len > 0);

  cJSON *root = parse_buf(buf, len);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "found") == 0);
  cJSON_Delete(root);
}

// ============================================================================
// encode_status
// ============================================================================

TEST_CASE("BleCodec: encode status notification", "[ble_codec]") {
  uint8_t buf[64];
  size_t len = BleCodec::encode_status(0, buf, sizeof(buf));
  REQUIRE(len > 0);

  cJSON *root = parse_buf(buf, len);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "status") == 0);
  cJSON_Delete(root);
}

TEST_CASE("BleCodec: encode status with code 10", "[ble_codec]") {
  uint8_t buf[64];
  size_t len = BleCodec::encode_status(10, buf, sizeof(buf));
  REQUIRE(len > 0);

  cJSON *root = parse_buf(buf, len);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "status") == 10);
  cJSON_Delete(root);
}

TEST_CASE("BleCodec: encode functions reject null buffer", "[ble_codec]") {
  REQUIRE(BleCodec::encode_scan_empty(nullptr, 0) == 0);
  REQUIRE(BleCodec::encode_status(0, nullptr, 0) == 0);
  REQUIRE(BleCodec::encode_scan_page(nullptr, 0, 1, 1, 0, nullptr, 0) == 0);
}
