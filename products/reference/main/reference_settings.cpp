#include "reference_settings.h"

namespace {

constexpr const char *KEY_MEASUREMENT_INTERVAL_SECONDS = "mis";
constexpr const char *KEY_LED_INDICATOR_ENABLED = "lie";
constexpr const char *KEY_DEVICE_NAME = "de";

bool is_measurement_interval_valid(int value) { return value >= 1 && value <= 3600; }

bool is_device_name_valid(const std::string &value) { return !value.empty() && value.size() <= 64; }

} // namespace

ReferenceSettings load_reference_settings(ConfigStore &store) {
  ReferenceSettings settings;

  int measurement_interval_seconds = 0;
  if (store.get_int(KEY_MEASUREMENT_INTERVAL_SECONDS, measurement_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_measurement_interval_valid(measurement_interval_seconds)) {
    settings.measurement_interval_seconds = measurement_interval_seconds;
  }

  bool led_indicator_enabled = false;
  if (store.get_bool(KEY_LED_INDICATOR_ENABLED, led_indicator_enabled) == ConfigStoreResult::OK) {
    settings.led_indicator_enabled = led_indicator_enabled;
  }

  std::string device_name;
  if (store.get_string(KEY_DEVICE_NAME, device_name) == ConfigStoreResult::OK &&
      is_device_name_valid(device_name)) {
    settings.device_name = device_name;
  }

  return settings;
}

bool save_reference_settings(ConfigStore &store, const ReferenceSettings &settings) {
  if (!is_measurement_interval_valid(settings.measurement_interval_seconds)) {
    return false;
  }

  if (!is_device_name_valid(settings.device_name)) {
    return false;
  }

  if (store.set_int(KEY_MEASUREMENT_INTERVAL_SECONDS, settings.measurement_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_LED_INDICATOR_ENABLED, settings.led_indicator_enabled) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_string(KEY_DEVICE_NAME, settings.device_name) != ConfigStoreResult::OK) {
    return false;
  }

  return store.commit() == ConfigStoreResult::OK;
}
