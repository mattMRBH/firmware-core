/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "internal/measures_json.h"

#include <cstring>

#include <cJSON.h>

namespace measures_json {

size_t serialize(const Measures &measures, const SystemInfo &info, char *buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }

  // Identity — always present.
  cJSON_AddStringToObject(root, "serial", info.serial_number);
  cJSON_AddStringToObject(root, "model", info.model);
  cJSON_AddStringToObject(root, "firmware", info.firmware);
  if (info.wifi_rssi.has_value()) {
    cJSON_AddNumberToObject(root, "wifi_rssi", static_cast<double>(*info.wifi_rssi));
  }

  // Measurements — emitted only when valid.
  if (measures.co2.is_valid()) {
    cJSON_AddNumberToObject(root, "co2", static_cast<double>(measures.co2.co2));
  }
  if (measures.pm_a.is_pm_01_valid()) {
    cJSON_AddNumberToObject(root, "pm01", static_cast<double>(measures.pm_a.pm_01));
  }
  if (measures.pm_a.is_pm_25_valid()) {
    cJSON_AddNumberToObject(root, "pm25", static_cast<double>(measures.pm_a.pm_25));
  }
  if (measures.pm_a.is_pm_10_valid()) {
    cJSON_AddNumberToObject(root, "pm10", static_cast<double>(measures.pm_a.pm_10));
  }
  if (measures.pm_a.is_pm_03_pc_valid()) {
    cJSON_AddNumberToObject(root, "pm003_count", static_cast<double>(measures.pm_a.pm_03_pc));
  }
  if (measures.temp_hum_a.is_temp_valid()) {
    cJSON_AddNumberToObject(root, "temp", static_cast<double>(measures.temp_hum_a.temperature));
  }
  if (measures.temp_hum_a.is_hum_valid()) {
    cJSON_AddNumberToObject(root, "humidity", static_cast<double>(measures.temp_hum_a.humidity));
  }
  if (measures.tvoc_nox.is_tvoc_index_valid()) {
    cJSON_AddNumberToObject(root, "tvoc_index", static_cast<double>(measures.tvoc_nox.tvoc_index));
  }
  if (measures.tvoc_nox.is_tvoc_raw_valid()) {
    cJSON_AddNumberToObject(root, "tvoc_raw", static_cast<double>(measures.tvoc_nox.tvoc_raw));
  }
  if (measures.tvoc_nox.is_nox_index_valid()) {
    cJSON_AddNumberToObject(root, "nox_index", static_cast<double>(measures.tvoc_nox.nox_index));
  }
  if (measures.tvoc_nox.is_nox_raw_valid()) {
    cJSON_AddNumberToObject(root, "nox_raw", static_cast<double>(measures.tvoc_nox.nox_raw));
  }

  const bool ok = cJSON_PrintPreallocated(root, buf, static_cast<int>(buf_len), /*format=*/0);
  cJSON_Delete(root);
  if (!ok) {
    return 0;
  }
  return std::strlen(buf);
}

} // namespace measures_json
