/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/scd4x/scd4x.h"

#include "esp_log.h"

#include "rtos.h"

#include "scd4x_i2c.h"
#include "sensirion_i2c_hal_esp_idf.h"

static constexpr const char *TAG = "SCD4x";

SCD4x::SCD4x(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _address(address), _measuring(false),
      _last_temp_hum{MeasuresInvalid::TEMPERATURE, MeasuresInvalid::HUMIDITY} {}

bool SCD4x::init() {
  // Probe I2C bus to verify device exists (with retry for boot timing).
  // Done with the ESP-IDF master-probe API directly so we fail fast without
  // touching the Sensirion HAL globals if the sensor is absent.
  bool probed = false;
  for (int i = 0; i < INIT_PROBE_RETRIES; i++) {
    esp_err_t ret = i2c_master_probe(_i2c_bus, _address, I2C_PROBE_TIMEOUT_MS);
    if (ret == ESP_OK) {
      probed = true;
      break;
    }
    ESP_LOGW(TAG, "Probe attempt %d/%d failed: %s", i + 1, INIT_PROBE_RETRIES,
             esp_err_to_name(ret));
    RTOS::delay_ms(INIT_PROBE_DELAY_MS);
  }
  if (!probed) {
    ESP_LOGE(TAG, "SCD4x not found at address 0x%02X", _address);
    return false;
  }

  ESP_LOGI(TAG, "SCD4x found at address 0x%02X", _address);

  // Bind the Sensirion HAL to our I2C bus and configure the driver's target
  // address. Note: these are file-scope globals in embedded-i2c-scd4x, so only
  // one SCD4x instance can be active at a time (see header doc).
  sensirion_i2c_hal_set_bus_handle(_i2c_bus);
  scd4x_init(_address);

  // Clean-state sequence (mirrors embedded-i2c-scd4x/example-usage):
  //   wake_up -> stop_periodic_measurement -> reinit
  // The wake_up command is not ACKed by SCD40, so ignore its return value.
  (void)scd4x_wake_up();
  RTOS::delay_ms(WAKE_UP_DELAY_MS);

  int16_t err = scd4x_stop_periodic_measurement();
  if (err != 0) {
    ESP_LOGW(TAG, "stop_periodic_measurement returned %d (ignored)", err);
  }
  // stop_periodic_measurement requires >500 ms before any further command.
  RTOS::delay_ms(STOP_PERIODIC_DELAY_MS);

  err = scd4x_reinit();
  if (err != 0) {
    ESP_LOGW(TAG, "reinit returned %d (ignored)", err);
  }

  // Best-effort diagnostics: log sensor variant and serial number.
  uint16_t variant_raw = 0;
  if (scd4x_get_sensor_variant_raw(&variant_raw) == 0) {
    const uint16_t variant_bits = variant_raw & SCD4X_SENSOR_VARIANT_MASK;
    const char *variant_name = "unknown";
    switch (variant_bits) {
    case SCD4X_SENSOR_VARIANT_SCD40:
      variant_name = "SCD40";
      break;
    case SCD4X_SENSOR_VARIANT_SCD41:
      variant_name = "SCD41";
      break;
    case SCD4X_SENSOR_VARIANT_SCD42:
      variant_name = "SCD42";
      break;
    case SCD4X_SENSOR_VARIANT_SCD43:
      variant_name = "SCD43";
      break;
    default:
      break;
    }
    ESP_LOGI(TAG, "variant: %s (raw=0x%04X)", variant_name, variant_raw);
  } else {
    ESP_LOGW(TAG, "get_sensor_variant_raw failed (ignored)");
  }

  uint16_t serial_words[SERIAL_NUMBER_WORD_COUNT] = {0};
  if (scd4x_get_serial_number(serial_words, SERIAL_NUMBER_WORD_COUNT) == 0) {
    ESP_LOGI(TAG, "serial: 0x%04X%04X%04X", serial_words[0], serial_words[1], serial_words[2]);
  } else {
    ESP_LOGW(TAG, "get_serial_number failed (ignored)");
  }

  // Start continuous (periodic) measurement mode with retry for transient
  // I2C failures.
  bool started = false;
  for (int i = 0; i < START_MEASUREMENT_RETRIES; i++) {
    err = scd4x_start_periodic_measurement();
    if (err == 0) {
      started = true;
      break;
    }
    ESP_LOGW(TAG, "start_periodic_measurement attempt %d/%d failed (err=%d)", i + 1,
             START_MEASUREMENT_RETRIES, err);
    RTOS::delay_ms(START_MEASUREMENT_RETRY_DELAY_MS);
  }
  if (!started) {
    ESP_LOGE(TAG, "Failed to start periodic measurement after %d attempts",
             START_MEASUREMENT_RETRIES);
    return false;
  }

  _measuring = true;
  ESP_LOGI(TAG, "SCD4x initialized, periodic measurement started");
  return true;
}

bool SCD4x::read(CO2Data &out) {
  // Initialize to invalid sentinel
  out.co2 = MeasuresInvalid::CO2;

  if (!_measuring) {
    ESP_LOGW(TAG, "Sensor not initialized or not measuring");
    return false;
  }

  for (int i = 0; i < READ_MEASUREMENT_RETRIES; i++) {
    bool data_ready = false;
    int16_t err = scd4x_get_data_ready_status(&data_ready);
    if (err != 0) {
      ESP_LOGW(TAG, "get_data_ready_status attempt %d/%d failed (err=%d)", i + 1,
               READ_MEASUREMENT_RETRIES, err);
    } else if (!data_ready) {
      ESP_LOGW(TAG, "Measurement not ready on attempt %d/%d", i + 1, READ_MEASUREMENT_RETRIES);
    } else {
      uint16_t co2_ppm = 0;
      int32_t temp_m_deg_c = 0;
      int32_t hum_m_percent = 0;
      err = scd4x_read_measurement(&co2_ppm, &temp_m_deg_c, &hum_m_percent);
      if (err == 0) {
        out.co2 = static_cast<int>(co2_ppm);

        _last_temp_hum.temperature = static_cast<float>(temp_m_deg_c) / 1000.0f;

        float hum = static_cast<float>(hum_m_percent) / 1000.0f;
        if (hum < 0.0f) {
          hum = 0.0f;
        }
        if (hum > 100.0f) {
          hum = 100.0f;
        }
        _last_temp_hum.humidity = hum;

        ESP_LOGD(TAG, "CO2: %d ppm, Temp: %.2f C, Hum: %.2f%%", out.co2, _last_temp_hum.temperature,
                 _last_temp_hum.humidity);
        return true;
      }

      ESP_LOGW(TAG, "read_measurement attempt %d/%d failed (err=%d)", i + 1,
               READ_MEASUREMENT_RETRIES, err);
    }

    if (i + 1 < READ_MEASUREMENT_RETRIES) {
      RTOS::delay_ms(READ_MEASUREMENT_RETRY_DELAY_MS);
    }
  }

  ESP_LOGE(TAG, "Failed to read measurement after %d attempts", READ_MEASUREMENT_RETRIES);
  return false;
}

bool SCD4x::supports_temp_hum() const { return true; }

TempHumData SCD4x::temp_hum_data() { return _last_temp_hum; }
