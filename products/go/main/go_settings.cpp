#include "go_settings.h"

namespace {

constexpr const char *KEY_MEASUREMENT_INTERVAL_SECONDS = "mis";
constexpr const char *KEY_DISPLAY_REFRESH_INTERVAL_SECONDS = "dri";
constexpr const char *KEY_INACTIVITY_TIMEOUT_SECONDS = "ito";
constexpr const char *KEY_GPS_INTERVAL_SECONDS = "gis";
constexpr const char *KEY_GPS_ENABLED = "gen";
constexpr const char *KEY_OPERATING_MODE = "opm";
constexpr const char *KEY_DEVICE_NAME = "dn";

bool is_measurement_interval_valid(int value) {
  return value >= 1 && value <= 3600;
}

bool is_display_refresh_interval_valid(int value) {
  return value >= 0 && value <= 3600;
}

bool is_inactivity_timeout_valid(int value) {
  return value >= 5 && value <= 600;
}

bool is_gps_interval_valid(int value) { return value >= 1 && value <= 60; }

bool is_operating_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_device_name_valid(const std::string &value) {
  return !value.empty() && value.size() <= 64;
}

} // namespace

GoSettings load_go_settings(ConfigStore &store) {
  GoSettings settings;

  int measurement_interval_seconds = 0;
  if (store.get_int(KEY_MEASUREMENT_INTERVAL_SECONDS,
                    measurement_interval_seconds) == ConfigStoreResult::OK &&
      is_measurement_interval_valid(measurement_interval_seconds)) {
    settings.measurement_interval_seconds = measurement_interval_seconds;
  }

  int display_refresh_interval_seconds = 0;
  if (store.get_int(KEY_DISPLAY_REFRESH_INTERVAL_SECONDS,
                    display_refresh_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_display_refresh_interval_valid(display_refresh_interval_seconds)) {
    settings.display_refresh_interval_seconds =
        display_refresh_interval_seconds;
  }

  int inactivity_timeout_seconds = 0;
  if (store.get_int(KEY_INACTIVITY_TIMEOUT_SECONDS,
                    inactivity_timeout_seconds) == ConfigStoreResult::OK &&
      is_inactivity_timeout_valid(inactivity_timeout_seconds)) {
    settings.inactivity_timeout_seconds = inactivity_timeout_seconds;
  }

  int gps_interval_seconds = 0;
  if (store.get_int(KEY_GPS_INTERVAL_SECONDS, gps_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_gps_interval_valid(gps_interval_seconds)) {
    settings.gps_interval_seconds = gps_interval_seconds;
  }

  bool gps_enabled = false;
  if (store.get_bool(KEY_GPS_ENABLED, gps_enabled) == ConfigStoreResult::OK) {
    settings.gps_enabled = gps_enabled;
  }

  int operating_mode = 0;
  if (store.get_int(KEY_OPERATING_MODE, operating_mode) ==
          ConfigStoreResult::OK &&
      is_operating_mode_valid(operating_mode)) {
    settings.operating_mode = static_cast<OperatingMode>(operating_mode);
  }

  std::string device_name;
  if (store.get_string(KEY_DEVICE_NAME, device_name) == ConfigStoreResult::OK &&
      is_device_name_valid(device_name)) {
    settings.device_name = device_name;
  }

  return settings;
}

bool save_go_settings(ConfigStore &store, const GoSettings &settings) {
  if (!is_measurement_interval_valid(settings.measurement_interval_seconds)) {
    return false;
  }

  if (!is_display_refresh_interval_valid(
          settings.display_refresh_interval_seconds)) {
    return false;
  }

  if (!is_inactivity_timeout_valid(settings.inactivity_timeout_seconds)) {
    return false;
  }

  if (!is_gps_interval_valid(settings.gps_interval_seconds)) {
    return false;
  }

  if (!is_operating_mode_valid(static_cast<int>(settings.operating_mode))) {
    return false;
  }

  if (!is_device_name_valid(settings.device_name)) {
    return false;
  }

  if (store.set_int(KEY_MEASUREMENT_INTERVAL_SECONDS,
                    settings.measurement_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_DISPLAY_REFRESH_INTERVAL_SECONDS,
                    settings.display_refresh_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_INACTIVITY_TIMEOUT_SECONDS,
                    settings.inactivity_timeout_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_GPS_INTERVAL_SECONDS, settings.gps_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_GPS_ENABLED, settings.gps_enabled) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_OPERATING_MODE,
                    static_cast<int>(settings.operating_mode)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_string(KEY_DEVICE_NAME, settings.device_name) !=
      ConfigStoreResult::OK) {
    return false;
  }

  return store.commit() == ConfigStoreResult::OK;
}
