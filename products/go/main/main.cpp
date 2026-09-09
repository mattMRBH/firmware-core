/**
 * AirGradient Go — Application Entry Point
 *
 * Thin shell: constructs the real board and runs the app.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_app.h"
#include "go_hardware_board.h"

// For ESP DFS and Auto Light Sleep (via FreeRTOS)
#include "esp_pm.h"
#include "esp_log.h"

static const char *TAG = "main";

extern "C" void app_main() {
  // Configure DFS and allow light sleep
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = 240;
  pm.min_freq_mhz = 80;
  pm.light_sleep_enable = true;

  esp_err_t err = esp_pm_configure(&pm);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Power Management succesfully enabled");
  } else if (err == ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG,
             "PM/DFS/Sleep not supported by current sdkconfig or target; continuing without it");
  } else {
    ESP_LOGE(TAG, "Power Management configuation failed: %s", esp_err_to_name(err));
  }

  GoHardwareBoard board;
  GoApp app(board);
  app.run();
}
