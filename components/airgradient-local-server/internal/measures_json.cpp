/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "internal/measures_json.h"

#include <cmath>
#include <cstring>

#include <cJSON.h>

#include "internal/field_names.h"

namespace {

// Keep local API precision aligned with the cloud measurement payload.
constexpr int DECIMALS_INT = 0;
constexpr int DECIMALS_PM_MASS = 1;
constexpr int DECIMALS_TEMP_HUM = 2;
constexpr int DECIMALS_VOLT = 2;

double round_to_decimals(double value, int decimals) {
  switch (decimals) {
  case 0:
    return std::round(value);
  case 1:
    return std::round(value * 10.0) / 10.0;
  case 2:
    return std::round(value * 100.0) / 100.0;
  default:
    return value;
  }
}

void add_float(cJSON *root, const char *name, float value, int decimals) {
  cJSON_AddNumberToObject(root, name, round_to_decimals(static_cast<double>(value), decimals));
}

bool is_finite(float value) { return std::isfinite(value); }

} // namespace

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
    add_float(root, fields::PM01, measures.pm_a.pm_01, DECIMALS_PM_MASS);
  }
  if (measures.pm_a.is_pm_25_valid()) {
    add_float(root, fields::PM25, measures.pm_a.pm_25, DECIMALS_PM_MASS);
  }
  if (measures.pm_a.is_pm_10_valid()) {
    add_float(root, fields::PM10, measures.pm_a.pm_10, DECIMALS_PM_MASS);
  }
  if (measures.pm_a.is_pm_03_pc_valid() && is_finite(measures.pm_a.pm_03_pc)) {
    add_float(root, fields::PM003_COUNT, measures.pm_a.pm_03_pc, DECIMALS_INT);
  }
  if (measures.pm_a.is_pm_05_pc_valid() && is_finite(measures.pm_a.pm_05_pc)) {
    add_float(root, fields::PM005_COUNT, measures.pm_a.pm_05_pc, DECIMALS_INT);
  }
  if (measures.pm_a.is_pm_01_pc_valid() && is_finite(measures.pm_a.pm_01_pc)) {
    add_float(root, fields::PM01_COUNT, measures.pm_a.pm_01_pc, DECIMALS_INT);
  }
  if (measures.pm_a.is_pm_25_pc_valid() && is_finite(measures.pm_a.pm_25_pc)) {
    add_float(root, fields::PM02_COUNT, measures.pm_a.pm_25_pc, DECIMALS_INT);
  }
  if (measures.pm_a.is_pm_5_pc_valid() && is_finite(measures.pm_a.pm_5_pc)) {
    add_float(root, fields::PM50_COUNT, measures.pm_a.pm_5_pc, DECIMALS_INT);
  }
  if (measures.pm_a.is_pm_10_pc_valid() && is_finite(measures.pm_a.pm_10_pc)) {
    add_float(root, fields::PM10_COUNT, measures.pm_a.pm_10_pc, DECIMALS_INT);
  }
  if (measures.temp_hum_a.is_temp_valid()) {
    add_float(root, fields::TEMPERATURE, measures.temp_hum_a.temperature, DECIMALS_TEMP_HUM);
  }
  if (measures.temp_hum_a.is_hum_valid()) {
    add_float(root, fields::HUMIDITY, measures.temp_hum_a.humidity, DECIMALS_TEMP_HUM);
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
  if (measures.power.is_battery_percentage_valid() &&
      is_finite(measures.power.battery_percentage)) {
    add_float(root, fields::BATT_PERCENT, measures.power.battery_percentage, DECIMALS_INT);
  }
  if (measures.power.is_battery_voltage_valid() && is_finite(measures.power.battery_voltage)) {
    add_float(root, fields::BATT_VOLT, measures.power.battery_voltage, DECIMALS_VOLT);
  }
  if (measures.power.is_charging_voltage_valid() && is_finite(measures.power.charging_voltage)) {
    add_float(root, fields::CHARGE_VOLT, measures.power.charging_voltage, DECIMALS_VOLT);
  }

  const bool ok = cJSON_PrintPreallocated(root, buf, static_cast<int>(buf_len), /*format=*/0);
  cJSON_Delete(root);
  if (!ok) {
    return 0;
  }
  return std::strlen(buf);
}

} // namespace measures_json
