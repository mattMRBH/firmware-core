/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "internal/measures_json.h"

#include <cstring>

#include <cJSON.h>

#include "internal/field_names.h"

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
  cJSON_AddStringToObject(root, fields::SERIAL_NUMBER, info.serial_number);
  cJSON_AddStringToObject(root, fields::MODEL, info.model);
  cJSON_AddStringToObject(root, fields::FIRMWARE, info.firmware);
  if (info.wifi_rssi.has_value()) {
    cJSON_AddNumberToObject(root, fields::WIFI_RSSI, static_cast<double>(*info.wifi_rssi));
  }
  cJSON_AddNumberToObject(root, fields::BOOT, static_cast<double>(info.boot));

  // Measurements — emitted only when valid.
  if (measures.co2.is_valid()) {
    cJSON_AddNumberToObject(root, fields::CO2, static_cast<double>(measures.co2.co2));
  }
  if (measures.pm_a.is_pm_01_valid()) {
    cJSON_AddNumberToObject(root, fields::PM01, static_cast<double>(measures.pm_a.pm_01));
  }
  if (measures.pm_a.is_pm_25_valid()) {
    cJSON_AddNumberToObject(root, fields::PM25, static_cast<double>(measures.pm_a.pm_25));
  }
  if (measures.pm_a.is_pm_10_valid()) {
    cJSON_AddNumberToObject(root, fields::PM10, static_cast<double>(measures.pm_a.pm_10));
  }
  if (measures.pm_a.is_pm_03_pc_valid()) {
    cJSON_AddNumberToObject(root, fields::PM003_COUNT, static_cast<double>(measures.pm_a.pm_03_pc));
  }
  if (measures.temp_hum_a.is_temp_valid()) {
    cJSON_AddNumberToObject(root, fields::TEMP,
                            static_cast<double>(measures.temp_hum_a.temperature));
  }
  if (measures.temp_hum_a.is_hum_valid()) {
    cJSON_AddNumberToObject(root, fields::HUMIDITY,
                            static_cast<double>(measures.temp_hum_a.humidity));
  }
  if (measures.tvoc_nox.is_tvoc_index_valid()) {
    cJSON_AddNumberToObject(root, fields::TVOC_INDEX,
                            static_cast<double>(measures.tvoc_nox.tvoc_index));
  }
  if (measures.tvoc_nox.is_tvoc_raw_valid()) {
    cJSON_AddNumberToObject(root, fields::TVOC_RAW,
                            static_cast<double>(measures.tvoc_nox.tvoc_raw));
  }
  if (measures.tvoc_nox.is_nox_index_valid()) {
    cJSON_AddNumberToObject(root, fields::NOX_INDEX,
                            static_cast<double>(measures.tvoc_nox.nox_index));
  }
  if (measures.tvoc_nox.is_nox_raw_valid()) {
    cJSON_AddNumberToObject(root, fields::NOX_RAW, static_cast<double>(measures.tvoc_nox.nox_raw));
  }

  const bool ok = cJSON_PrintPreallocated(root, buf, static_cast<int>(buf_len), /*format=*/0);
  cJSON_Delete(root);
  if (!ok) {
    return 0;
  }
  return std::strlen(buf);
}

} // namespace measures_json
