/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "ble_codec.h"

#include <cstring>

#include <cJSON.h>

#include "ag_log.h"
#include "ip_utils.h"

namespace {

constexpr const char *TAG = "BleCodec";

// Render a cJSON tree into a byte buffer. Returns bytes written, 0 on error.
// Caller must cJSON_Delete(root) afterward.
size_t render_json(cJSON *root, uint8_t *buf, size_t buf_size) {
  char *encoded = cJSON_PrintUnformatted(root);
  if (encoded == nullptr) {
    return 0;
  }
  size_t len = std::strlen(encoded);
  if (len >= buf_size) {
    AG_LOGW(TAG, "JSON output (%u) exceeds buffer (%u)", static_cast<unsigned>(len),
            static_cast<unsigned>(buf_size));
    cJSON_free(encoded);
    return 0;
  }
  std::memcpy(buf, encoded, len);
  cJSON_free(encoded);
  return len;
}

} // namespace

bool BleCodec::parse_credentials(const uint8_t *data, size_t len, ProvisioningData &out) {
  if (data == nullptr || len == 0) {
    return false;
  }

  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(data), len);
  if (root == nullptr) {
    return false;
  }

  cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
  if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr || ssid->valuestring[0] == '\0') {
    cJSON_Delete(root);
    return false;
  }

  out = ProvisioningData{};
  std::strncpy(out.ssid, ssid->valuestring, sizeof(out.ssid) - 1);

  cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
  if (cJSON_IsString(password) && password->valuestring != nullptr) {
    std::strncpy(out.password, password->valuestring, sizeof(out.password) - 1);
  }

  cJSON *disable_cloud = cJSON_GetObjectItemCaseSensitive(root, "disableCloud");
  if (cJSON_IsBool(disable_cloud)) {
    out.disable_cloud = cJSON_IsTrue(disable_cloud);
  }

  cJSON *static_ip = cJSON_GetObjectItemCaseSensitive(root, "staticIp");
  if (cJSON_IsObject(static_ip)) {
    cJSON *ip_node = cJSON_GetObjectItemCaseSensitive(static_ip, "ip");
    cJSON *netmask_node = cJSON_GetObjectItemCaseSensitive(static_ip, "netmask");
    cJSON *gateway_node = cJSON_GetObjectItemCaseSensitive(static_ip, "gateway");
    cJSON *dns_node = cJSON_GetObjectItemCaseSensitive(static_ip, "dns");

    if (cJSON_IsString(ip_node) && ip_node->valuestring != nullptr) {
      parse_ipv4(ip_node->valuestring, out.static_ip.ip);
    }
    if (cJSON_IsString(netmask_node) && netmask_node->valuestring != nullptr) {
      parse_ipv4(netmask_node->valuestring, out.static_ip.netmask);
    }
    if (cJSON_IsString(gateway_node) && gateway_node->valuestring != nullptr) {
      parse_ipv4(gateway_node->valuestring, out.static_ip.gateway);
    }
    if (cJSON_IsString(dns_node) && dns_node->valuestring != nullptr) {
      parse_ipv4(dns_node->valuestring, out.static_ip.dns_primary);
    }
  }

  cJSON_Delete(root);
  return true;
}

size_t BleCodec::encode_scan_page(const WifiScanEntry *entries, size_t entries_count, size_t page,
                                  size_t total_pages, size_t total_found, uint8_t *buf,
                                  size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }

  cJSON *wifi = cJSON_CreateArray();
  if (wifi == nullptr) {
    cJSON_Delete(root);
    return 0;
  }

  for (size_t i = 0; i < entries_count; ++i) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == nullptr) {
      break;
    }
    cJSON_AddStringToObject(entry, "s", entries[i].ssid);
    cJSON_AddNumberToObject(entry, "r", static_cast<double>(entries[i].rssi));
    // "o" = 1 for open, 0 for secured
    int open_flag = (entries[i].auth_mode == WifiAuthMode::open) ? 1 : 0;
    cJSON_AddNumberToObject(entry, "o", static_cast<double>(open_flag));
    cJSON_AddItemToArray(wifi, entry);
  }

  cJSON_AddItemToObject(root, "wifi", wifi);
  cJSON_AddNumberToObject(root, "page", static_cast<double>(page));
  cJSON_AddNumberToObject(root, "tpage", static_cast<double>(total_pages));
  cJSON_AddNumberToObject(root, "found", static_cast<double>(total_found));

  size_t written = render_json(root, buf, buf_size);
  cJSON_Delete(root);
  return written;
}

size_t BleCodec::encode_scan_empty(uint8_t *buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }
  cJSON_AddNumberToObject(root, "found", 0);

  size_t written = render_json(root, buf, buf_size);
  cJSON_Delete(root);
  return written;
}

size_t BleCodec::encode_status(uint8_t status_code, uint8_t *buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }
  cJSON_AddNumberToObject(root, "status", static_cast<double>(status_code));

  size_t written = render_json(root, buf, buf_size);
  cJSON_Delete(root);
  return written;
}
