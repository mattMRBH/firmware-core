/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "internal/config_json.h"

#include <cstring>

#include <cJSON.h>

namespace config_json {

namespace {

// Enum value sets. Kept identical to the existing Home Assistant integration.
constexpr const char *PM_STANDARD_VALUES[] = {"ugm3", "us-aqi"};
constexpr const char *TEMP_UNIT_VALUES[] = {"c", "f"};
constexpr const char *CONFIG_CONTROL_VALUES[] = {"cloud", "local", "both"};
constexpr const char *LED_BAR_MODE_VALUES[] = {"off", "co2", "pm"};

bool is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

template <size_t N> bool enum_contains(const char *value, const char *const (&options)[N]) {
  for (size_t i = 0; i < N; ++i) {
    if (std::strcmp(value, options[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Validate one known string field; returns true and assigns on success.
bool take_string(const cJSON *item, std::optional<std::string> &out) {
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  out = item->valuestring;
  return true;
}

template <size_t N>
bool take_enum(const cJSON *item, const char *const (&options)[N],
               std::optional<std::string> &out) {
  if (!cJSON_IsString(item) || item->valuestring == nullptr ||
      !enum_contains(item->valuestring, options)) {
    return false;
  }
  out = item->valuestring;
  return true;
}

bool take_int(const cJSON *item, std::optional<int> &out) {
  if (!cJSON_IsNumber(item)) {
    return false;
  }
  out = item->valueint;
  return true;
}

bool take_bool(const cJSON *item, std::optional<bool> &out) {
  if (!cJSON_IsBool(item)) {
    return false;
  }
  out = cJSON_IsTrue(item) != 0;
  return true;
}

// Apply a single object member. Returns Ok / InvalidValue / UnknownField and,
// on a known-field type error, sets `field`.
ParseStatus apply_item(const cJSON *item, LocalServerConfig &out, ConfigFieldId &field) {
  const char *key = item->string;
  if (key == nullptr) {
    return ParseStatus::UnknownField;
  }

  if (std::strcmp(key, "country") == 0) {
    field = ConfigFieldId::CountryCode;
    return take_string(item, out.country) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "pm_standard") == 0) {
    field = ConfigFieldId::PmStandard;
    return take_enum(item, PM_STANDARD_VALUES, out.pm_standard) ? ParseStatus::Ok
                                                                : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "temp_unit") == 0) {
    field = ConfigFieldId::TempUnit;
    return take_enum(item, TEMP_UNIT_VALUES, out.temp_unit) ? ParseStatus::Ok
                                                            : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "cloud_enabled") == 0) {
    field = ConfigFieldId::CloudEnabled;
    return take_bool(item, out.cloud_enabled) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "configuration_control") == 0) {
    field = ConfigFieldId::ConfigurationControl;
    return take_enum(item, CONFIG_CONTROL_VALUES, out.configuration_control)
               ? ParseStatus::Ok
               : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "co2_calib_days") == 0) {
    field = ConfigFieldId::Co2CalibDays;
    return take_int(item, out.co2_calib_days) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "tvoc_offset") == 0) {
    field = ConfigFieldId::TvocOffset;
    return take_int(item, out.tvoc_offset) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "nox_offset") == 0) {
    field = ConfigFieldId::NoxOffset;
    return take_int(item, out.nox_offset) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "led_bar_mode") == 0) {
    field = ConfigFieldId::LedBarMode;
    return take_enum(item, LED_BAR_MODE_VALUES, out.led_bar_mode) ? ParseStatus::Ok
                                                                  : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "led_bar_brightness") == 0) {
    field = ConfigFieldId::LedBarBrightness;
    return take_int(item, out.led_bar_brightness) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, "display_brightness") == 0) {
    field = ConfigFieldId::DisplayBrightness;
    return take_int(item, out.display_brightness) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }

  return ParseStatus::UnknownField;
}

} // namespace

ParseResult parse(const char *body, size_t len, LocalServerConfig &out) {
  ParseResult result;
  if (body == nullptr || len == 0) {
    result.status = ParseStatus::InvalidBody;
    return result;
  }

  const char *parse_end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(body, len, &parse_end, /*require_null_terminated=*/0);
  if (root == nullptr) {
    result.status = ParseStatus::InvalidBody;
    return result;
  }

  // Reject a non-object root (array, string, number, etc.).
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    result.status = ParseStatus::InvalidBody;
    return result;
  }

  // Reject trailing non-whitespace after the root object.
  const char *end = body + len;
  for (const char *p = parse_end; p != nullptr && p < end; ++p) {
    if (!is_whitespace(*p)) {
      cJSON_Delete(root);
      result.status = ParseStatus::InvalidBody;
      return result;
    }
  }

  for (const cJSON *item = root->child; item != nullptr; item = item->next) {
    ConfigFieldId field = ConfigFieldId::None;
    const ParseStatus status = apply_item(item, out, field);
    if (status == ParseStatus::UnknownField) {
      result.status = ParseStatus::UnknownField;
      if (item->string != nullptr) {
        std::strncpy(result.unknown_key, item->string, MAX_UNKNOWN_KEY - 1);
        result.unknown_key[MAX_UNKNOWN_KEY - 1] = '\0';
      }
      cJSON_Delete(root);
      return result;
    }
    if (status == ParseStatus::InvalidValue) {
      result.status = ParseStatus::InvalidValue;
      result.field = field;
      cJSON_Delete(root);
      return result;
    }
  }

  cJSON_Delete(root);
  result.status = ParseStatus::Ok;
  return result;
}

size_t serialize(const LocalServerConfig &cfg, char *buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }

  if (cfg.country.has_value()) {
    cJSON_AddStringToObject(root, "country", cfg.country->c_str());
  }
  if (cfg.pm_standard.has_value()) {
    cJSON_AddStringToObject(root, "pm_standard", cfg.pm_standard->c_str());
  }
  if (cfg.temp_unit.has_value()) {
    cJSON_AddStringToObject(root, "temp_unit", cfg.temp_unit->c_str());
  }
  if (cfg.cloud_enabled.has_value()) {
    cJSON_AddBoolToObject(root, "cloud_enabled", *cfg.cloud_enabled);
  }
  if (cfg.configuration_control.has_value()) {
    cJSON_AddStringToObject(root, "configuration_control", cfg.configuration_control->c_str());
  }
  if (cfg.co2_calib_days.has_value()) {
    cJSON_AddNumberToObject(root, "co2_calib_days", static_cast<double>(*cfg.co2_calib_days));
  }
  if (cfg.tvoc_offset.has_value()) {
    cJSON_AddNumberToObject(root, "tvoc_offset", static_cast<double>(*cfg.tvoc_offset));
  }
  if (cfg.nox_offset.has_value()) {
    cJSON_AddNumberToObject(root, "nox_offset", static_cast<double>(*cfg.nox_offset));
  }
  if (cfg.led_bar_mode.has_value()) {
    cJSON_AddStringToObject(root, "led_bar_mode", cfg.led_bar_mode->c_str());
  }
  if (cfg.led_bar_brightness.has_value()) {
    cJSON_AddNumberToObject(root, "led_bar_brightness",
                            static_cast<double>(*cfg.led_bar_brightness));
  }
  if (cfg.display_brightness.has_value()) {
    cJSON_AddNumberToObject(root, "display_brightness",
                            static_cast<double>(*cfg.display_brightness));
  }

  const bool ok = cJSON_PrintPreallocated(root, buf, static_cast<int>(buf_len), /*format=*/0);
  cJSON_Delete(root);
  if (!ok) {
    return 0;
  }
  return std::strlen(buf);
}

const char *config_field_wire_key(ConfigFieldId id) {
  switch (id) {
  case ConfigFieldId::CountryCode:
    return "country";
  case ConfigFieldId::PmStandard:
    return "pm_standard";
  case ConfigFieldId::TempUnit:
    return "temp_unit";
  case ConfigFieldId::CloudEnabled:
    return "cloud_enabled";
  case ConfigFieldId::ConfigurationControl:
    return "configuration_control";
  case ConfigFieldId::Co2CalibDays:
    return "co2_calib_days";
  case ConfigFieldId::TvocOffset:
    return "tvoc_offset";
  case ConfigFieldId::NoxOffset:
    return "nox_offset";
  case ConfigFieldId::LedBarMode:
    return "led_bar_mode";
  case ConfigFieldId::LedBarBrightness:
    return "led_bar_brightness";
  case ConfigFieldId::DisplayBrightness:
    return "display_brightness";
  case ConfigFieldId::None:
    return nullptr;
  }
  return nullptr;
}

} // namespace config_json
