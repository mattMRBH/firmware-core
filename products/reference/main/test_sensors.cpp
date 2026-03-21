#include "test_sensors.h"

#include "esp_log.h"

#include "airgradient_iic_serial.h"
#include "airgradient_uart.h"
#include "board_config.h"
#include "drivers/pms5003/pms5003.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sht40/sht40.h"
#include "drivers/sunlight/sunlight.h"
#include "measures_types.h"
#include "rtos.h"

static constexpr const char *TAG = "test_sensors";

static constexpr int  N_READINGS      = 5;
static constexpr int  READING_INTERVAL_MS = 2000;
static constexpr int  SGP41_COND_CYCLES   = 10;  // ~5 s conditioning

void run_test_sensors(i2c_master_bus_handle_t i2c_bus) {
  ESP_LOGI(TAG, "--- Sensors test start (%d readings) ---", N_READINGS);

  // Serial transports
  AirgradientIICSerial pms1_serial(
      i2c_bus, AirgradientIICSerial::SUB_UART_CHANNEL_1, 0, 1, PIN_IIC_RESET);
  AirgradientIICSerial pms2_serial(
      i2c_bus, AirgradientIICSerial::SUB_UART_CHANNEL_2, 0, 1, PIN_IIC_RESET);
  AirgradientUART co2_serial(UART_PORT_CO2, PIN_UART_RX_CO2, PIN_UART_TX_CO2);

  if (!pms1_serial.begin(9600) || !pms2_serial.begin(9600)) {
    ESP_LOGE(TAG, "IIC serial begin failed");
    return;
  }
  if (!co2_serial.begin(UART_BAUD_CO2)) {
    ESP_LOGE(TAG, "CO2 UART begin failed");
    return;
  }

  // Sensor instances
  PMS5003  pms1(pms1_serial);
  PMS5003  pms2(pms2_serial);
  Sunlight co2(co2_serial, Sunlight::MODE_SINGLE, PIN_EN_CO2);
  SHT40    sht40(i2c_bus);
  SGP41    sgp41(i2c_bus);

  if (!pms1.init()) {
    ESP_LOGW(TAG, "PMS1 init failed — skipping");
  }
  if (!pms2.init()) {
    ESP_LOGW(TAG, "PMS2 init failed — skipping");
  }
  if (!co2.init()) {
    ESP_LOGW(TAG, "CO2 init failed — skipping");
  }
  if (!sht40.init()) {
    ESP_LOGW(TAG, "SHT40 init failed — skipping");
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
    if (pms1.read(pm_data) && pm_data.is_pm_25_valid()) {
      ESP_LOGI(TAG, "PMS1  PM2.5=%.1f PM10=%.1f ug/m3", pm_data.pm_25, pm_data.pm_10);
    }
    if (pms2.read(pm_data) && pm_data.is_pm_25_valid()) {
      ESP_LOGI(TAG, "PMS2  PM2.5=%.1f PM10=%.1f ug/m3", pm_data.pm_25, pm_data.pm_10);
    }

    TempHumData th_data;
    if (sht40.read(th_data)) {
      if (th_data.is_temp_valid()) {
        ESP_LOGI(TAG, "SHT40 temp=%.2f°C hum=%.2f%%", th_data.temperature, th_data.humidity);
      }
    }

    CO2Data co2_data;
    if (co2.read(co2_data) && co2_data.is_valid()) {
      ESP_LOGI(TAG, "CO2   %d ppm", co2_data.co2);
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
