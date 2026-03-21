#ifndef REFERENCE_SETTINGS_H
#define REFERENCE_SETTINGS_H

#include <string>

#include "config_store.h"

struct ReferenceSettings {
  int measurement_interval_seconds = 60;
  bool led_indicator_enabled = true;
  std::string device_name = "airgradient-reference";
};

ReferenceSettings load_reference_settings(ConfigStore &store);
bool save_reference_settings(ConfigStore &store,
                             const ReferenceSettings &settings);

#endif // REFERENCE_SETTINGS_H
