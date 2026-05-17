/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "payload_serializer.h"

#include <cstring>
#include <type_traits>

#include "cJSON.h"

// Property names match the AirGradient server contract (see spec.md).
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

// SFINAE helpers to detect optional substructs on the Measures variant in use.
template <typename, typename = void> struct has_temp_hum_b : std::false_type {};
template <typename T>
struct has_temp_hum_b<T, std::void_t<decltype(std::declval<T &>().temp_hum_b)>> : std::true_type {};

template <typename, typename = void> struct has_pm_b : std::false_type {};
template <typename T>
struct has_pm_b<T, std::void_t<decltype(std::declval<T &>().pm_b)>> : std::true_type {};

template <typename, typename = void> struct has_power : std::false_type {};
template <typename T>
struct has_power<T, std::void_t<decltype(std::declval<T &>().power)>> : std::true_type {};

template <typename, typename = void> struct has_electrode : std::false_type {};
template <typename T>
struct has_electrode<T, std::void_t<decltype(std::declval<T &>().electrode)>> : std::true_type {};

// Add a number to cJSON object only when the boolean predicate holds.  cJSON
// renders integers up to 2^53; sensor values are well within that range.
inline void add_int(cJSON *obj, const char *name, int value) {
  cJSON_AddNumberToObject(obj, name, static_cast<double>(value));
}
inline void add_float(cJSON *obj, const char *name, float value) {
  cJSON_AddNumberToObject(obj, name, static_cast<double>(value));
}

// Dual-channel merge: average when both valid, otherwise pick valid one,
// otherwise omit.
template <typename ValidFn, typename T>
void add_avg_if_valid(cJSON *obj, const char *name, T a, T b, ValidFn is_valid) {
  const bool a_ok = is_valid(a);
  const bool b_ok = is_valid(b);
  if (a_ok && b_ok) {
    cJSON_AddNumberToObject(obj, name, (static_cast<double>(a) + static_cast<double>(b)) / 2.0);
  } else if (a_ok) {
    cJSON_AddNumberToObject(obj, name, static_cast<double>(a));
  } else if (b_ok) {
    cJSON_AddNumberToObject(obj, name, static_cast<double>(b));
  }
}

// Single-channel: include only when valid.
template <typename ValidFn, typename T>
void add_if_valid(cJSON *obj, const char *name, T value, ValidFn is_valid) {
  if (is_valid(value)) {
    cJSON_AddNumberToObject(obj, name, static_cast<double>(value));
  }
}

template <typename T> void serialize_temp_hum(cJSON *obj, const T &m) {
  if constexpr (has_temp_hum_b<T>::value) {
    add_avg_if_valid(
        obj, JSON_PROP_TEMP, m.temp_hum_a.temperature, m.temp_hum_b.temperature, [&](float v) {
          return v >= MeasuresRange::MIN_VALID_TEMP && v <= MeasuresRange::MAX_VALID_TEMP;
        });
    add_avg_if_valid(
        obj, JSON_PROP_RHUM, m.temp_hum_a.humidity, m.temp_hum_b.humidity, [&](float v) {
          return v >= MeasuresRange::MIN_VALID_HUM && v <= MeasuresRange::MAX_VALID_HUM;
        });
  } else {
    if (m.temp_hum_a.is_temp_valid()) {
      add_float(obj, JSON_PROP_TEMP, m.temp_hum_a.temperature);
    }
    if (m.temp_hum_a.is_hum_valid()) {
      add_float(obj, JSON_PROP_RHUM, m.temp_hum_a.humidity);
    }
  }
}

template <typename T> void serialize_pm(cJSON *obj, const T &m) {
  auto pm_valid = [](float v) { return v >= MeasuresRange::MIN_VALID_PM; };
  if constexpr (has_pm_b<T>::value) {
    add_avg_if_valid(obj, JSON_PROP_PM01, m.pm_a.pm_01, m.pm_b.pm_01, pm_valid);
    add_avg_if_valid(obj, JSON_PROP_PM25, m.pm_a.pm_25, m.pm_b.pm_25, pm_valid);
    add_avg_if_valid(obj, JSON_PROP_PM10, m.pm_a.pm_10, m.pm_b.pm_10, pm_valid);
    add_avg_if_valid(obj, JSON_PROP_PM03_COUNT, m.pm_a.pm_03_pc, m.pm_b.pm_03_pc, pm_valid);
  } else {
    add_if_valid(obj, JSON_PROP_PM01, m.pm_a.pm_01, pm_valid);
    add_if_valid(obj, JSON_PROP_PM25, m.pm_a.pm_25, pm_valid);
    add_if_valid(obj, JSON_PROP_PM10, m.pm_a.pm_10, pm_valid);
    add_if_valid(obj, JSON_PROP_PM03_COUNT, m.pm_a.pm_03_pc, pm_valid);
  }
}

template <typename T> void serialize_co2(cJSON *obj, const T &m) {
  if (m.co2.is_valid()) {
    add_int(obj, JSON_PROP_CO2, m.co2.co2);
  }
}

template <typename T> void serialize_tvoc_nox(cJSON *obj, const T &m) {
  if (m.tvoc_nox.is_tvoc_index_valid()) {
    add_int(obj, JSON_PROP_TVOC, m.tvoc_nox.tvoc_index);
  }
  if (m.tvoc_nox.is_tvoc_raw_valid()) {
    add_int(obj, JSON_PROP_TVOC_RAW, m.tvoc_nox.tvoc_raw);
  }
  if (m.tvoc_nox.is_nox_index_valid()) {
    add_int(obj, JSON_PROP_NOX, m.tvoc_nox.nox_index);
  }
  if (m.tvoc_nox.is_nox_raw_valid()) {
    add_int(obj, JSON_PROP_NOX_RAW, m.tvoc_nox.nox_raw);
  }
}

template <typename T> void serialize_power(cJSON *obj, const T &m) {
  if constexpr (has_power<T>::value) {
    if (m.power.is_battery_voltage_valid()) {
      add_float(obj, JSON_PROP_VBATT, m.power.battery_voltage);
    }
    if (m.power.is_charging_voltage_valid()) {
      add_float(obj, JSON_PROP_VPANEL, m.power.charging_voltage);
    }
  } else {
    (void)m;
  }
}

template <typename T> void serialize_electrode(cJSON *obj, const T &m) {
  if constexpr (has_electrode<T>::value) {
    if (m.electrode.is_o3_working_valid()) {
      add_float(obj, JSON_PROP_O3_WE, m.electrode.o3_we);
    }
    if (m.electrode.is_o3_auxiliary_valid()) {
      add_float(obj, JSON_PROP_O3_AE, m.electrode.o3_ae);
    }
    if (m.electrode.is_no2_working_valid()) {
      add_float(obj, JSON_PROP_NO2_WE, m.electrode.no2_we);
    }
    if (m.electrode.is_no2_auxiliary_valid()) {
      add_float(obj, JSON_PROP_NO2_AE, m.electrode.no2_ae);
    }
    if (m.electrode.is_afe_temp_valid()) {
      add_float(obj, JSON_PROP_AFE_TEMP, m.electrode.afe_temp);
    }
  } else {
    (void)m;
  }
}

} // namespace

namespace ag_client {

bool serialize_measures_json(const AgClientMeasuresType &measures, int signal, char *out,
                             size_t out_size, size_t *bytes_written) {
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

  // Signal is always included (spec).
  add_int(doc, JSON_PROP_SIGNAL, signal);

  serialize_co2(doc, measures);
  serialize_temp_hum(doc, measures);
  serialize_pm(doc, measures);
  serialize_tvoc_nox(doc, measures);
  serialize_power(doc, measures);
  serialize_electrode(doc, measures);

  // Render unformatted (compact) JSON into caller's buffer.
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

} // namespace ag_client
