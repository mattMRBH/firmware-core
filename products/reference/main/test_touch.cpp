#include "test_touch.h"

#include "esp_log.h"

#include "board_config.h"
#include "cap1203.h"
#include "cap_touch_sensor.h"
#include "rtos.h"

static constexpr const char *TAG = "test_touch";

static constexpr int N_READINGS        = 200;
static constexpr int READING_INTERVAL_MS = 200;

void run_test_touch(i2c_master_bus_handle_t i2c_bus) {
  ESP_LOGI(TAG, "--- Touch test start (%d readings) ---", N_READINGS);

  CAP1203::Config touch_cfg;
  touch_cfg.delta_sense = TOUCH_DELTA_SENSE;
  CAP1203 touch(i2c_bus, CAP1203::DEFAULT_ADDRESS, touch_cfg);

  if (!touch.init()) {
    ESP_LOGW(TAG, "CAP1203 init failed — is it connected?");
    return;
  }
  ESP_LOGI(TAG, "CAP1203 ready (noise=%s calibration=%s)",
           touch.supports_noise() ? "yes" : "no",
           touch.supports_calibration() ? "yes" : "no");

  if (touch.supports_calibration()) {
    touch.calibrate(TouchChannel::ALL);
    ESP_LOGI(TAG, "calibration triggered");
    RTOS::delay_ms(100);
  }

  for (int i = 0; i < N_READINGS; i++) {
    TouchData data;
    if (!touch.read(data)) {
      ESP_LOGW(TAG, "[%d] read failed", i);
    } else if (data.touched != 0) {
      ESP_LOGI(TAG, "[%d] touched: CH1=%d CH2=%d CH3=%d", i,
               (data.touched & TouchChannel::CH1) != 0,
               (data.touched & TouchChannel::CH2) != 0,
               (data.touched & TouchChannel::CH3) != 0);
    } else {
      ESP_LOGI(TAG, "[%d] no touch", i);
    }

    if (touch.supports_noise() && data.noise != 0) {
      ESP_LOGW(TAG, "[%d] noise mask: 0x%02X", i, data.noise);
    }

    if (i < N_READINGS - 1) {
      RTOS::delay_ms(READING_INTERVAL_MS);
    }
  }

  ESP_LOGI(TAG, "--- Touch test done ---");
}
