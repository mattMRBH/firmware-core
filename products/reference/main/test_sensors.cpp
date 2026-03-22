#include "test_sensors.h"

#include "esp_log.h"

#include "board_config.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sps30/sps30.h"
#include "drivers/stcc4/stcc4.h"
#include "measures_types.h"
#include "rtos.h"

static constexpr const char *TAG = "test_sensors";

static constexpr int N_READINGS = 5;
static constexpr int READING_INTERVAL_MS = 2000;
static constexpr int SGP41_COND_CYCLES = 10; // ~5 s conditioning

void run_test_sensors(i2c_master_bus_handle_t i2c_bus) {
  ESP_LOGI(TAG, "--- Sensors test start (%d readings) ---", N_READINGS);

  // Sensor instances (all I2C)
  SPS30 sps30(i2c_bus);
  STCC4 stcc4(i2c_bus);
  SGP41 sgp41(i2c_bus);

  if (!sps30.init()) {
    ESP_LOGW(TAG, "SPS30 init failed — skipping");
  }
  if (!stcc4.init()) {
    ESP_LOGW(TAG, "STCC4 init failed — skipping");
  }
  if (!sgp41.init()) {
    ESP_LOGW(TAG, "SGP41 init failed — skipping");
  } else {
    ESP_LOGI(TAG, "SGP41 conditioning (%d cycles)...", SGP41_COND_CYCLES);
    for (int i = 0; i < SGP41_COND_CYCLES; i++) {
      if (!sgp41.run_conditioning()) {
        ESP_LOGW(TAG, "SGP41 conditioning failed at cycle %d", i + 1);
      }
      RTOS::delay_ms(500);
    }
  }

  for (int reading = 1; reading <= N_READINGS; reading++) {
    ESP_LOGI(TAG, "=== reading %d/%d ===", reading, N_READINGS);

    PMData pm_data;
    if (sps30.read(pm_data) && pm_data.is_pm_25_valid()) {
      ESP_LOGI(TAG, "SPS30  PM1.0=%.1f PM2.5=%.1f PM10=%.1f ug/m3", pm_data.pm_01, pm_data.pm_25,
               pm_data.pm_10);
    }

    CO2Data co2_data;
    if (stcc4.read(co2_data) && co2_data.is_valid()) {
      ESP_LOGI(TAG, "STCC4 CO2=%d ppm", co2_data.co2);

      // STCC4 also provides temperature and humidity
      TempHumData th_data = stcc4.temp_hum_data();
      if (th_data.is_temp_valid()) {
        ESP_LOGI(TAG, "STCC4 temp=%.2f°C hum=%.2f%%", th_data.temperature, th_data.humidity);
      }
    }

    TVOCNOxData voc_data;
    if (sgp41.read(voc_data)) {
      if (voc_data.is_tvoc_raw_valid()) {
        ESP_LOGI(TAG, "SGP41 TVOC=%d NOx=%d (raw)", voc_data.tvoc_raw, voc_data.nox_raw);
      }
    }

    if (reading < N_READINGS) {
      RTOS::delay_ms(READING_INTERVAL_MS);
    }
  }

  ESP_LOGI(TAG, "--- Sensors test done ---");
}
