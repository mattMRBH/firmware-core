#ifndef GO_SETTINGS_H
#define GO_SETTINGS_H

#include <string>

#include "config_store.h"
#include "go_types.h"
#include "led/go_led_types.h"
#include "types/wifi_types.h"

struct GoSettings {
  // --- Measurement interval ---
  int measure_interval_seconds = 10; // 1..3600

  // --- Display ---
  bool use_fahrenheit = false;
  bool pm_use_usaqi = false;

  // --- GPS ---
  int gps_interval_seconds = 5;
  GpsMode gps_mode = GpsMode::OnWhenTracking;

  // --- Device behavior ---
  OperatingMode operating_mode = OperatingMode::Portable;
  int inactivity_timeout_seconds = 5;
  int auto_lock_seconds = 0; // 0 = auto-lock disabled

  // --- Identity ---
  std::string device_name = "airgradient-go";

  // --- LED brightness ---
  LedBrightness front_led_brightness = LedBrightness::Bright;        // Scopes 2-3 default
  LedBrightness back_led_brightness = LedBrightness::Bright;         // Scopes 2-3 default
  TouchLedIntensity touch_led_intensity = TouchLedIntensity::Bright; // Scopes 2-3 default

  // --- Stationary connectivity ---
  bool disable_cloud = false;     // honored by CloudService
  WifiStaticIpConfig static_ip{}; // ip == 0 means DHCP
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);
void print_settings(const GoSettings &settings);

#endif // GO_SETTINGS_H
