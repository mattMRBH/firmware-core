/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/alpha_sense/alpha_sense.h"
#include "esp_log.h"
#include "measures_types.h"
#include "rtos.h"

static constexpr const char *TAG = "AlphaSense";

AlphaSense::AlphaSense(i2c_master_bus_handle_t i2c_bus, uint8_t gas_addr, uint8_t temp_addr)
    : _i2c_bus(i2c_bus), _gas_addr(gas_addr), _temp_addr(temp_addr) {}

bool AlphaSense::init() {
  // Initialize gas sensor ADC (ADS1115 #1)
  _ads_gas = std::make_unique<ADS1115>(_gas_addr);
  if (_ads_gas->init(_i2c_bus) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize gas ADC at 0x%02X", _gas_addr);
    _ads_gas.reset();
    return false;
  }

  // Set voltage range to ±4096mV for gas electrodes
  if (!_ads_gas->setVoltageRange_mV(ADS1115_RANGE_4096)) {
    ESP_LOGE(TAG, "Failed to set voltage range for gas ADC");
    return false;
  }

  ESP_LOGI(TAG, "Gas ADC initialized at 0x%02X", _gas_addr);

  // Initialize temperature sensor ADC (ADS1115 #2)
  _ads_temp = std::make_unique<ADS1115>(_temp_addr);
  if (_ads_temp->init(_i2c_bus) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize temperature ADC at 0x%02X", _temp_addr);
    _ads_temp.reset();
    return false;
  }

  // Set voltage range to ±4096mV for temperature reading
  if (!_ads_temp->setVoltageRange_mV(ADS1115_RANGE_4096)) {
    ESP_LOGE(TAG, "Failed to set voltage range for temperature ADC");
    return false;
  }

  ESP_LOGI(TAG, "Temperature ADC initialized at 0x%02X", _temp_addr);

  return true;
}

bool AlphaSense::read(O3No2Data &out) {
  // Initialize to invalid sentinels
  out.o3_we = MeasuresInvalid::VOLT;
  out.o3_ae = MeasuresInvalid::VOLT;
  out.no2_we = MeasuresInvalid::VOLT;
  out.no2_ae = MeasuresInvalid::VOLT;
  out.afe_temp = MeasuresInvalid::VOLT;

  // Check if ADCs are initialized
  if (_ads_gas == nullptr || _ads_temp == nullptr) {
    ESP_LOGE(TAG, "ADCs not initialized");
    return false;
  }

  // Read gas electrodes from ADS1115 #1
  out.no2_we = _read_channel(_ads_gas.get(), ADS1115_COMP_0_GND);
  if (out.no2_we < 0) {
    ESP_LOGW(TAG, "Failed to read NO2 working electrode");
  }

  out.no2_ae = _read_channel(_ads_gas.get(), ADS1115_COMP_1_GND);
  if (out.no2_ae < 0) {
    ESP_LOGW(TAG, "Failed to read NO2 auxiliary electrode");
  }

  out.o3_we = _read_channel(_ads_gas.get(), ADS1115_COMP_2_GND);
  if (out.o3_we < 0) {
    ESP_LOGW(TAG, "Failed to read O3 working electrode");
  }

  out.o3_ae = _read_channel(_ads_gas.get(), ADS1115_COMP_3_GND);
  if (out.o3_ae < 0) {
    ESP_LOGW(TAG, "Failed to read O3 auxiliary electrode");
  }

  // Read temperature from ADS1115 #2
  out.afe_temp = _read_channel(_ads_temp.get(), ADS1115_COMP_0_GND);
  if (out.afe_temp < 0) {
    ESP_LOGW(TAG, "Failed to read AFE temperature");
  }

  // Return true if at least one valid reading obtained
  return out.is_o3_working_valid() || out.is_o3_auxiliary_valid() || out.is_no2_working_valid() ||
         out.is_no2_auxiliary_valid() || out.is_afe_temp_valid();
}

float AlphaSense::_read_channel(ADS1115 *adc, ADS1115_MUX channel) {
  if (adc == nullptr) {
    ESP_LOGE(TAG, "ADC pointer is null");
    return -1.0f;
  }

  // Set channel to read
  if (!adc->setCompareChannels(channel)) {
    ESP_LOGE(TAG, "Failed to set ADC channel 0x%04X", channel);
    return -1.0f;
  }

  // Start single measurement
  if (!adc->startSingleMeasurement()) {
    ESP_LOGE(TAG, "Failed to start measurement on channel 0x%04X", channel);
    return -1.0f;
  }

  // Wait for conversion to complete (poll busy flag)
  while (adc->isBusy()) {
    RTOS::delay_ms(1);
  }

  // Read result in millivolts
  float voltage_mv = adc->getResult_mV();

  return voltage_mv;
}
