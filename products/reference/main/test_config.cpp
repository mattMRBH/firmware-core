#include "test_config.h"

#include "esp_log.h"

#include "reference_settings.h"

static constexpr const char *TAG = "test_config";

void run_test_config(ConfigStore &store) {
  ESP_LOGI(TAG, "--- Config test start ---");

  // Load current settings.
  ReferenceSettings original = load_reference_settings(store);
  ESP_LOGI(TAG, "loaded: interval=%d led=%s name=%s", original.measurement_interval_seconds,
           original.led_indicator_enabled ? "true" : "false", original.device_name.c_str());

  // Mutate and save.
  ReferenceSettings modified = original;
  modified.measurement_interval_seconds = original.measurement_interval_seconds + 1;
  modified.led_indicator_enabled = !original.led_indicator_enabled;
  modified.device_name = "test-device";

  if (!save_reference_settings(store, modified)) {
    ESP_LOGE(TAG, "save failed");
    return;
  }
  ESP_LOGI(TAG, "saved modified settings");

  // Reload and verify round-trip.
  ReferenceSettings reloaded = load_reference_settings(store);
  const bool interval_ok =
      reloaded.measurement_interval_seconds == modified.measurement_interval_seconds;
  const bool led_ok = reloaded.led_indicator_enabled == modified.led_indicator_enabled;
  const bool name_ok = reloaded.device_name == modified.device_name;

  ESP_LOGI(TAG, "round-trip: interval=%s led=%s name=%s", interval_ok ? "OK" : "FAIL",
           led_ok ? "OK" : "FAIL", name_ok ? "OK" : "FAIL");

  // Restore original.
  save_reference_settings(store, original);
  ESP_LOGI(TAG, "original settings restored");

  ESP_LOGI(TAG, "--- Config test done ---");
}
