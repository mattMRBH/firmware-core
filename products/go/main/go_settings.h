#ifndef GO_SETTINGS_H
#define GO_SETTINGS_H

#include <string>

#include "config_store.h"
#include "go_types.h"

struct GoSettings {
  // --- Sensor intervals ---
  int measurement_interval_seconds = 60;
  int pm_interval_seconds = 10;           // 0 = PM sensor off
  int other_sensor_interval_seconds = 10; // 0 = other sensors off

  // --- Display ---
  int display_refresh_interval_seconds = 60; // 0 = display off
  bool use_fahrenheit = false;
  bool pm_use_usaqi = false;

  // --- GPS ---
  int gps_interval_seconds = 5;
  GpsMode gps_mode = GpsMode::OnWhenTracking;

  // --- Device behavior ---
  OperatingMode operating_mode = OperatingMode::Offline;
  int inactivity_timeout_seconds = 30;
  int auto_lock_seconds = 0; // 0 = auto-lock disabled

  // --- Identity ---
  std::string device_name = "airgradient-go";
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);

#endif // GO_SETTINGS_H
