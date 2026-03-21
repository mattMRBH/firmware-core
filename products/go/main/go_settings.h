#ifndef GO_SETTINGS_H
#define GO_SETTINGS_H

#include <string>

#include "config_store.h"
#include "go_types.h"

struct GoSettings {
  int measurement_interval_seconds = 60;
  int display_refresh_interval_seconds = 60; // 0 = display refresh disabled
  int inactivity_timeout_seconds = 30;
  int gps_interval_seconds = 5;
  bool gps_enabled = true;
  OperatingMode operating_mode = OperatingMode::Offline;
  std::string device_name = "airgradient-go";
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);

#endif // GO_SETTINGS_H
