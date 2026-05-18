/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "payload_serializer.h"

#include <cstring>

#include "cJSON.h"

// Property names match the AirGradient server contract.
namespace {

constexpr const char *JSON_PROP_SIGNAL = "wifi";
constexpr const char *JSON_PROP_CO2 = "rco2";
constexpr const char *JSON_PROP_TEMP = "atmp";
constexpr const char *JSON_PROP_RHUM = "rhum";
constexpr const char *JSON_PROP_PM01 = "pm01";
constexpr const char *JSON_PROP_PM25 = "pm02";
constexpr const char *JSON_PROP_PM10 = "pm10";
constexpr const char *JSON_PROP_PM03_COUNT = "pm003Count";
constexpr const char *JSON_PROP_TVOC = "tvocIndex";
constexpr const char *JSON_PROP_TVOC_RAW = "tvocRaw";
constexpr const char *JSON_PROP_NOX = "noxIndex";
constexpr const char *JSON_PROP_NOX_RAW = "noxRaw";
constexpr const char *JSON_PROP_VBATT = "volt";
constexpr const char *JSON_PROP_VPANEL = "light";
constexpr const char *JSON_PROP_O3_WE = "measure0";
constexpr const char *JSON_PROP_O3_AE = "measure1";
constexpr const char *JSON_PROP_NO2_WE = "measure2";
constexpr const char *JSON_PROP_NO2_AE = "measure3";
constexpr const char *JSON_PROP_AFE_TEMP = "measure4";

inline void add_int(cJSON *obj, const char *name, int value) {
  cJSON_AddNumberToObject(obj, name, static_cast<double>(value));
}

inline void add_float(cJSON *obj, const char *name, float value) {
  cJSON_AddNumberToObject(obj, name, static_cast<double>(value));
}

// Mean if both valid, single value if one, omit if neither.
void emit_dual_float(cJSON *obj, const char *name, bool a_ok, float a_val, bool b_ok, float b_val) {
  if (a_ok && b_ok) {
    add_float(obj, name, (a_val + b_val) / 2.0f);
  } else if (a_ok) {
    add_float(obj, name, a_val);
  } else if (b_ok) {
    add_float(obj, name, b_val);
  }
}

void serialize_co2(cJSON *obj, const CO2Data *co2) {
  if (co2 != nullptr && co2->is_valid()) {
    add_int(obj, JSON_PROP_CO2, co2->co2);
  }
}

void serialize_temp_hum(cJSON *obj, const TempHumData *a, const TempHumData *b) {
  const bool a_temp = (a != nullptr) && a->is_temp_valid();
  const bool b_temp = (b != nullptr) && b->is_temp_valid();
  emit_dual_float(obj, JSON_PROP_TEMP, a_temp, a_temp ? a->temperature : 0.0f, b_temp,
                  b_temp ? b->temperature : 0.0f);

  const bool a_hum = (a != nullptr) && a->is_hum_valid();
  const bool b_hum = (b != nullptr) && b->is_hum_valid();
  emit_dual_float(obj, JSON_PROP_RHUM, a_hum, a_hum ? a->humidity : 0.0f, b_hum,
                  b_hum ? b->humidity : 0.0f);
}

void serialize_pm(cJSON *obj, const PMData *a, const PMData *b) {
  const bool a01 = (a != nullptr) && a->is_pm_01_valid();
  const bool b01 = (b != nullptr) && b->is_pm_01_valid();
  emit_dual_float(obj, JSON_PROP_PM01, a01, a01 ? a->pm_01 : 0.0f, b01, b01 ? b->pm_01 : 0.0f);

  const bool a25 = (a != nullptr) && a->is_pm_25_valid();
  const bool b25 = (b != nullptr) && b->is_pm_25_valid();
  emit_dual_float(obj, JSON_PROP_PM25, a25, a25 ? a->pm_25 : 0.0f, b25, b25 ? b->pm_25 : 0.0f);

  const bool a10 = (a != nullptr) && a->is_pm_10_valid();
  const bool b10 = (b != nullptr) && b->is_pm_10_valid();
  emit_dual_float(obj, JSON_PROP_PM10, a10, a10 ? a->pm_10 : 0.0f, b10, b10 ? b->pm_10 : 0.0f);

  const bool a03 = (a != nullptr) && a->is_pm_03_pc_valid();
  const bool b03 = (b != nullptr) && b->is_pm_03_pc_valid();
  emit_dual_float(obj, JSON_PROP_PM03_COUNT, a03, a03 ? a->pm_03_pc : 0.0f, b03,
                  b03 ? b->pm_03_pc : 0.0f);
}

void serialize_tvoc_nox(cJSON *obj, const TVOCNOxData *t) {
  if (t == nullptr) {
    return;
  }
  if (t->is_tvoc_index_valid()) {
    add_int(obj, JSON_PROP_TVOC, t->tvoc_index);
  }
  if (t->is_tvoc_raw_valid()) {
    add_int(obj, JSON_PROP_TVOC_RAW, t->tvoc_raw);
  }
  if (t->is_nox_index_valid()) {
    add_int(obj, JSON_PROP_NOX, t->nox_index);
  }
  if (t->is_nox_raw_valid()) {
    add_int(obj, JSON_PROP_NOX_RAW, t->nox_raw);
  }
}

void serialize_power(cJSON *obj, const MeasuresPower *p) {
  if (p == nullptr) {
    return;
  }
  if (p->is_battery_voltage_valid()) {
    add_float(obj, JSON_PROP_VBATT, p->battery_voltage);
  }
  if (p->is_charging_voltage_valid()) {
    add_float(obj, JSON_PROP_VPANEL, p->charging_voltage);
  }
}

void serialize_electrode(cJSON *obj, const O3No2Data *e) {
  if (e == nullptr) {
    return;
  }
  if (e->is_o3_working_valid()) {
    add_float(obj, JSON_PROP_O3_WE, e->o3_we);
  }
  if (e->is_o3_auxiliary_valid()) {
    add_float(obj, JSON_PROP_O3_AE, e->o3_ae);
  }
  if (e->is_no2_working_valid()) {
    add_float(obj, JSON_PROP_NO2_WE, e->no2_we);
  }
  if (e->is_no2_auxiliary_valid()) {
    add_float(obj, JSON_PROP_NO2_AE, e->no2_ae);
  }
  if (e->is_afe_temp_valid()) {
    add_float(obj, JSON_PROP_AFE_TEMP, e->afe_temp);
  }
}

} // namespace

bool serialize_measures_json(const MeasuresInput &input, int signal, char *out, size_t out_size,
                             size_t *bytes_written) {
  if (bytes_written != nullptr) {
    *bytes_written = 0;
  }
  if (out == nullptr || out_size == 0) {
    return false;
  }

  cJSON *doc = cJSON_CreateObject();
  if (doc == nullptr) {
    return false;
  }

  add_int(doc, JSON_PROP_SIGNAL, signal); // always included

  serialize_co2(doc, input.co2);
  serialize_temp_hum(doc, input.temp_hum_a, input.temp_hum_b);
  serialize_pm(doc, input.pm_a, input.pm_b);
  serialize_tvoc_nox(doc, input.tvoc_nox);
  serialize_power(doc, input.power);
  serialize_electrode(doc, input.electrode);

  const bool ok = cJSON_PrintPreallocated(doc, out, static_cast<int>(out_size), /*fmt=*/0);
  cJSON_Delete(doc);

  if (!ok) {
    out[0] = '\0';
    return false;
  }

  if (bytes_written != nullptr) {
    *bytes_written = std::strlen(out);
  }
  return true;
}
