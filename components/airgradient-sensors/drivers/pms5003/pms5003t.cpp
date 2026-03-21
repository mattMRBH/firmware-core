/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/pms5003/pms5003t.h"
#include "esp_log.h"

static constexpr const char *TAG = "PMS5003T";

PMS5003T::PMS5003T(AirgradientSerial &serial) : PMS5003Base(serial) {
  // Initialize to invalid sentinels
  _last_temp_hum.temperature = MeasuresInvalid::TEMPERATURE;
  _last_temp_hum.humidity = MeasuresInvalid::HUMIDITY;
}

bool PMS5003T::read(PMData &out) {
  // Initialize to invalid sentinels
  out.pm_01 = MeasuresInvalid::PM;
  out.pm_25 = MeasuresInvalid::PM;
  out.pm_10 = MeasuresInvalid::PM;
  out.pm_01_sp = MeasuresInvalid::PM;
  out.pm_25_sp = MeasuresInvalid::PM;
  out.pm_10_sp = MeasuresInvalid::PM;
  out.pm_03_pc = MeasuresInvalid::PM;
  out.pm_05_pc = MeasuresInvalid::PM;
  out.pm_01_pc = MeasuresInvalid::PM;
  out.pm_25_pc = MeasuresInvalid::PM;
  out.pm_5_pc = MeasuresInvalid::PM;
  out.pm_10_pc = MeasuresInvalid::PM;

  _last_temp_hum.temperature = MeasuresInvalid::TEMPERATURE;
  _last_temp_hum.humidity = MeasuresInvalid::HUMIDITY;

  // Request and read frame
  clear_buffer();
  request_read();
  if (!read_frame(1000)) {
    ESP_LOGW(TAG, "Failed to read frame");
    return false;
  }

  // Parse PM data from payload
  // Standard Particles, CF=1 (bytes 0-5)
  out.pm_01_sp = static_cast<float>(_make_word(_payload[0], _payload[1]));
  out.pm_25_sp = static_cast<float>(_make_word(_payload[2], _payload[3]));
  out.pm_10_sp = static_cast<float>(_make_word(_payload[4], _payload[5]));

  // Atmospheric Environment (bytes 6-11)
  out.pm_01 = static_cast<float>(_make_word(_payload[6], _payload[7]));
  out.pm_25 = static_cast<float>(_make_word(_payload[8], _payload[9]));
  out.pm_10 = static_cast<float>(_make_word(_payload[10], _payload[11]));

  // Particle counts per 0.1L air (bytes 12-19)
  // Note: PMS5003T only has 4 particle count fields (0.3, 0.5, 1.0, 2.5 μm)
  out.pm_03_pc = static_cast<float>(_make_word(_payload[12], _payload[13]));
  out.pm_05_pc = static_cast<float>(_make_word(_payload[14], _payload[15]));
  out.pm_01_pc = static_cast<float>(_make_word(_payload[16], _payload[17]));
  out.pm_25_pc = static_cast<float>(_make_word(_payload[18], _payload[19]));
  // pm_5_pc and pm_10_pc remain invalid for PMS5003T

  // Temperature and Humidity (bytes 20-23)
  // Temperature: signed int16, unit: 0.1°C
  int16_t temp_raw =
      static_cast<int16_t>(_make_word(_payload[20], _payload[21]));
  _last_temp_hum.temperature = temp_raw / 10.0f;

  // Humidity: unsigned uint16, unit: 0.1%
  uint16_t hum_raw = _make_word(_payload[22], _payload[23]);
  _last_temp_hum.humidity = hum_raw / 10.0f;

  ESP_LOGD(TAG, "PM1.0=%0.1f PM2.5=%0.1f PM10=%0.1f T=%0.1f°C RH=%0.1f%%",
           out.pm_01, out.pm_25, out.pm_10, _last_temp_hum.temperature,
           _last_temp_hum.humidity);

  return true;
}

bool PMS5003T::supports_temp_hum() const { return true; }

TempHumData PMS5003T::temp_hum_data() { return _last_temp_hum; }
