#include "go_settings.h"

namespace {

constexpr const char *KEY_MEASUREMENT_INTERVAL_SECONDS = "mis";
constexpr const char *KEY_DISPLAY_REFRESH_INTERVAL_SECONDS = "dri";
constexpr const char *KEY_INACTIVITY_TIMEOUT_SECONDS = "ito";
constexpr const char *KEY_GPS_INTERVAL_SECONDS = "gis";
constexpr const char *KEY_GPS_MODE = "gpm";
constexpr const char *KEY_OPERATING_MODE = "opm";
constexpr const char *KEY_DEVICE_NAME = "dn";
constexpr const char *KEY_USE_FAHRENHEIT = "uf";
constexpr const char *KEY_PM_USE_USAQI = "pmu";
constexpr const char *KEY_PM_INTERVAL_SECONDS = "pis";
constexpr const char *KEY_OTHER_SENSOR_INTERVAL_SECONDS = "ois";
constexpr const char *KEY_AUTO_LOCK_SECONDS = "als";

bool is_measurement_interval_valid(int value) { return value >= 1 && value <= 3600; }

bool is_display_refresh_interval_valid(int value) { return value >= 0 && value <= 3600; }

bool is_inactivity_timeout_valid(int value) { return value >= 5 && value <= 600; }

bool is_gps_interval_valid(int value) { return value >= 1 && value <= 60; }

bool is_gps_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_operating_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_sensor_interval_valid(int value) {
  return value >= 0 && value <= 3600; // 0 = off
}

bool is_auto_lock_valid(int value) {
  return value == 0 || value == 10 || value == 30 || value == 60;
}

bool is_device_name_valid(const std::string &value) { return !value.empty() && value.size() <= 64; }

} // namespace

GoSettings load_go_settings(ConfigStore &store) {
  GoSettings settings;

  int measurement_interval_seconds = 0;
  if (store.get_int(KEY_MEASUREMENT_INTERVAL_SECONDS, measurement_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_measurement_interval_valid(measurement_interval_seconds)) {
    settings.measurement_interval_seconds = measurement_interval_seconds;
  }

  int display_refresh_interval_seconds = 0;
  if (store.get_int(KEY_DISPLAY_REFRESH_INTERVAL_SECONDS, display_refresh_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_display_refresh_interval_valid(display_refresh_interval_seconds)) {
    settings.display_refresh_interval_seconds = display_refresh_interval_seconds;
  }

  int inactivity_timeout_seconds = 0;
  if (store.get_int(KEY_INACTIVITY_TIMEOUT_SECONDS, inactivity_timeout_seconds) ==
          ConfigStoreResult::OK &&
      is_inactivity_timeout_valid(inactivity_timeout_seconds)) {
    settings.inactivity_timeout_seconds = inactivity_timeout_seconds;
  }

  int gps_interval_seconds = 0;
  if (store.get_int(KEY_GPS_INTERVAL_SECONDS, gps_interval_seconds) == ConfigStoreResult::OK &&
      is_gps_interval_valid(gps_interval_seconds)) {
    settings.gps_interval_seconds = gps_interval_seconds;
  }

  int gps_mode = 0;
  if (store.get_int(KEY_GPS_MODE, gps_mode) == ConfigStoreResult::OK &&
      is_gps_mode_valid(gps_mode)) {
    settings.gps_mode = static_cast<GpsMode>(gps_mode);
  }

  int operating_mode = 0;
  if (store.get_int(KEY_OPERATING_MODE, operating_mode) == ConfigStoreResult::OK &&
      is_operating_mode_valid(operating_mode)) {
    settings.operating_mode = static_cast<OperatingMode>(operating_mode);
  }

  std::string device_name;
  if (store.get_string(KEY_DEVICE_NAME, device_name) == ConfigStoreResult::OK &&
      is_device_name_valid(device_name)) {
    settings.device_name = device_name;
  }

  bool use_fahrenheit = false;
  if (store.get_bool(KEY_USE_FAHRENHEIT, use_fahrenheit) == ConfigStoreResult::OK) {
    settings.use_fahrenheit = use_fahrenheit;
  }

  bool pm_use_usaqi = false;
  if (store.get_bool(KEY_PM_USE_USAQI, pm_use_usaqi) == ConfigStoreResult::OK) {
    settings.pm_use_usaqi = pm_use_usaqi;
  }

  int pm_interval_seconds = 0;
  if (store.get_int(KEY_PM_INTERVAL_SECONDS, pm_interval_seconds) == ConfigStoreResult::OK &&
      is_sensor_interval_valid(pm_interval_seconds)) {
    settings.pm_interval_seconds = pm_interval_seconds;
  }

  int other_sensor_interval_seconds = 0;
  if (store.get_int(KEY_OTHER_SENSOR_INTERVAL_SECONDS, other_sensor_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_sensor_interval_valid(other_sensor_interval_seconds)) {
    settings.other_sensor_interval_seconds = other_sensor_interval_seconds;
  }

  int auto_lock_seconds = 0;
  if (store.get_int(KEY_AUTO_LOCK_SECONDS, auto_lock_seconds) == ConfigStoreResult::OK &&
      is_auto_lock_valid(auto_lock_seconds)) {
    settings.auto_lock_seconds = auto_lock_seconds;
  }

  return settings;
}

bool save_go_settings(ConfigStore &store, const GoSettings &settings) {
  if (!is_measurement_interval_valid(settings.measurement_interval_seconds)) {
    return false;
  }

  if (!is_display_refresh_interval_valid(settings.display_refresh_interval_seconds)) {
    return false;
  }

  if (!is_inactivity_timeout_valid(settings.inactivity_timeout_seconds)) {
    return false;
  }

  if (!is_gps_interval_valid(settings.gps_interval_seconds)) {
    return false;
  }

  if (!is_gps_mode_valid(static_cast<int>(settings.gps_mode))) {
    return false;
  }

  if (!is_operating_mode_valid(static_cast<int>(settings.operating_mode))) {
    return false;
  }

  if (!is_device_name_valid(settings.device_name)) {
    return false;
  }

  if (!is_sensor_interval_valid(settings.pm_interval_seconds)) {
    return false;
  }

  if (!is_sensor_interval_valid(settings.other_sensor_interval_seconds)) {
    return false;
  }

  if (store.set_int(KEY_MEASUREMENT_INTERVAL_SECONDS, settings.measurement_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_DISPLAY_REFRESH_INTERVAL_SECONDS,
                    settings.display_refresh_interval_seconds) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_INACTIVITY_TIMEOUT_SECONDS, settings.inactivity_timeout_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_GPS_INTERVAL_SECONDS, settings.gps_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_OPERATING_MODE, static_cast<int>(settings.operating_mode)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_string(KEY_DEVICE_NAME, settings.device_name) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_USE_FAHRENHEIT, settings.use_fahrenheit) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_PM_USE_USAQI, settings.pm_use_usaqi) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_PM_INTERVAL_SECONDS, settings.pm_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_OTHER_SENSOR_INTERVAL_SECONDS, settings.other_sensor_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_AUTO_LOCK_SECONDS, settings.auto_lock_seconds) != ConfigStoreResult::OK) {
    return false;
  }

  return store.commit() == ConfigStoreResult::OK;
}
