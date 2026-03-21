/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq25xx/bq25xx.h"
#include "esp_log.h"
#include "measures_types.h"
#include "rtos.h"

static constexpr const char *TAG = "BQ25XX";

BQ25XX::BQ25XX(i2c_master_bus_handle_t i2cBus, uint8_t address)
    : _i2cBus(i2cBus), _devHandle(nullptr), _address(address),
      _lastUpdateTimeMs(0) {}

bool BQ25XX::init() {
  // Probe I2C bus to verify device presence
  esp_err_t err = i2c_master_probe(_i2cBus, _address, 1000);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "BQ25XX not found at address 0x%.2x", _address);
    return false;
  }

  ESP_LOGI(TAG, "BQ25XX detected at address 0x%.2x", _address);

  // Configure I2C device
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = 500000, // 500kHz
      .scl_wait_us = 0,
      .flags = {},
  };

  err = i2c_master_bus_add_device(_i2cBus, &dev_cfg, &_devHandle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
    return false;
  }

  // Enable MPPT
  if (!_writeRegister(REG_MPPT, MPPT_ENABLE_VALUE)) {
    ESP_LOGE(TAG, "Failed to enable MPPT");
    return false;
  }

  // Verify MPPT enabled
  uint16_t mpptValue = 0;
  if (!_readRegister(REG_MPPT, 1, &mpptValue)) {
    ESP_LOGE(TAG, "Failed to read MPPT status");
    return false;
  }

  ESP_LOGI(TAG, "MPPT status: 0x%.2x", (uint8_t)mpptValue);
  if ((uint8_t)mpptValue != MPPT_ENABLE_VALUE) {
    ESP_LOGW(TAG, "MPPT status unexpected (0x%.2x), expected 0x%.2x",
             (uint8_t)mpptValue, MPPT_ENABLE_VALUE);
  }

  // Enable ADC with 15-bit resolution and Continuous Mode
  if (!_writeRegister(REG_ADC_CTRL, ADC_ENABLE_VALUE)) {
    ESP_LOGE(TAG, "Failed to enable ADC");
    return false;
  }

  // Verify ADC enabled
  uint16_t adcCtrl = 0;
  if (!_readRegister(REG_ADC_CTRL, 1, &adcCtrl)) {
    ESP_LOGE(TAG, "Failed to read ADC control register");
    return false;
  }

  if ((adcCtrl & 0x80) == 0) {
    ESP_LOGE(TAG, "ADC not enabled (0x%.2x)", (uint8_t)adcCtrl);
    return false;
  }

  ESP_LOGI(TAG, "BQ25XX initialized successfully");
  _lastUpdateTimeMs = RTOS::get_time_ms();

  return true;
}

bool BQ25XX::read(BatteryMgmtData &out) {
  // Initialize to invalid sentinels
  out.volt_battery = MeasuresInvalid::VOLT;
  out.volt_charging = MeasuresInvalid::VOLT;

  // Check device handle
  if (_devHandle == nullptr) {
    ESP_LOGE(TAG, "Device not initialized");
    return false;
  }

  // Update watchdog timer
  updateWatchdog();

  // Read battery voltage using getVBAT()
  uint16_t vbat = 0;
  if (!getVBAT(&vbat)) {
    ESP_LOGE(TAG, "Failed to read VBAT in read()");
    return false;
  }
  out.volt_battery = (float)vbat / 1000; // Convert from mV to V

  // Read charging voltage using getVBUS()
  uint16_t vbus = 0;
  if (!getVBUS(&vbus)) {
    ESP_LOGE(TAG, "Failed to read VBUS in read()");
    return false;
  }
  out.volt_charging = (float)vbus / 1000; // Convert from mV to V

  ESP_LOGD(TAG, "VBAT: %.0f mV, VBUS: %.0f mV", out.volt_battery,
           out.volt_charging);

  return true;
}

bool BQ25XX::updateWatchdog() {
  uint64_t currentTime = RTOS::get_time_ms();

  if ((currentTime - _lastUpdateTimeMs) > WATCHDOG_UPDATE_INTERVAL_MS) {
    // Attempt to enable MPPT and read status
    uint16_t mpptValue = 0;
    _writeRegister(REG_MPPT, MPPT_ENABLE_VALUE);
    _readRegister(REG_MPPT, 1, &mpptValue);
    ESP_LOGD(TAG, "MPPT Status: 0x%.2x", (uint8_t)mpptValue);

    // Write to ADC_CTRL register to reset watchdog timer
    if (_writeRegister(REG_ADC_CTRL, ADC_ENABLE_VALUE)) {
      _lastUpdateTimeMs = currentTime;
      ESP_LOGD(TAG, "Watchdog timer reset");
      return true;
    } else {
      ESP_LOGW(TAG, "Failed to reset watchdog timer");
      return false;
    }
  }

  return true; // No update needed yet
}

bool BQ25XX::getVBAT(uint16_t *output) {
  uint16_t result = 0;
  if (!getVBATRaw(&result)) {
    ESP_LOGE(TAG, "Failed to get VBAT");
    return false;
  }
  *output = result; // Direct mV conversion (1:1)
  return true;
}

bool BQ25XX::getVSYS(uint16_t *output) {
  uint16_t result = 0;
  if (!getVSYSRaw(&result)) {
    ESP_LOGE(TAG, "Failed to get VSYS");
    return false;
  }
  *output = result; // Direct mV conversion (1:1)
  return true;
}

bool BQ25XX::getVBUS(uint16_t *output) {
  uint16_t result = 0;
  if (!getVBUSRaw(&result)) {
    ESP_LOGE(TAG, "Failed to get VBUS");
    return false;
  }
  *output = result; // Direct mV conversion (1:1)
  return true;
}

bool BQ25XX::getBatteryPercentage(float *output) {
  uint16_t vbatAdc = 0;
  if (!getVBATRaw(&vbatAdc)) {
    ESP_LOGE(TAG, "Failed to get battery percentage");
    return false;
  }

  float vbat = (float)vbatAdc; // Convert to mV

  // Calculate percentage based on 9000-12600 mV range
  float batteryPercentage = ((vbat - 9000.0f) / (12600.0f - 9000.0f)) * 100.0f;

  // Clamp to 0-100%
  if (batteryPercentage > 100.0f) {
    batteryPercentage = 100.0f;
  } else if (batteryPercentage < 0.0f) {
    batteryPercentage = 0.0f;
  }

  *output = batteryPercentage;
  return true;
}

bool BQ25XX::getTemperature(float *output) {
  uint16_t temp = 0;
  if (!getTemperatureRaw(&temp)) {
    ESP_LOGE(TAG, "Failed to get temperature");
    return false;
  }
  *output = (float)temp;
  return true;
}

bool BQ25XX::getBatteryCurrent(int16_t *output) {
  uint16_t raw = 0;
  if (!getIBATRaw(&raw)) {
    ESP_LOGE(TAG, "Failed to get battery current");
    return false;
  }

  int16_t batteryCurrent = (int16_t)raw;

  // Convert 2's complement to signed integer
  if (batteryCurrent & 0x8000) {
    batteryCurrent = batteryCurrent - 0x10000;
  }

  *output = batteryCurrent;
  return true;
}

BQ25XX::ChargingStatus BQ25XX::getChargingStatus() {
  // Read Charger Status register
  uint16_t result = 0;
  if (!_readRegister(REG_CHG_STAT, 1, &result)) {
    ESP_LOGE(TAG, "Failed to get charging status");
    return ChargingStatus::Unknown;
  }

  // Extract CHG_STAT[7:5] (Bits 7 to 5)
  uint8_t chargingStatus = (uint8_t)result;
  uint8_t status = (chargingStatus >> 5) & 0x07;

  // Map status bits to enum
  ChargingStatus cs = ChargingStatus::Unknown;
  switch (status) {
  case 0b000:
    cs = ChargingStatus::NotCharging;
    ESP_LOGI(TAG, "Charging status: not charging");
    break;
  case 0b001:
    cs = ChargingStatus::TrickleCharge;
    ESP_LOGI(TAG, "Charging status: trickle charge");
    break;
  case 0b010:
    cs = ChargingStatus::PreCharge;
    ESP_LOGI(TAG, "Charging status: pre-charge");
    break;
  case 0b011:
    cs = ChargingStatus::FastCharge;
    ESP_LOGI(TAG, "Charging status: fast charge (CC Mode)");
    break;
  case 0b100:
    cs = ChargingStatus::TaperCharge;
    ESP_LOGI(TAG, "Charging status: taper charge (CV Mode)");
    break;
  case 0b110:
    cs = ChargingStatus::TopOffTimerActiveCharging;
    ESP_LOGI(TAG, "Charging status: top off timer active charging");
    break;
  case 0b111:
    cs = ChargingStatus::ChargeTerminationDone;
    ESP_LOGI(TAG, "Charging status: charge termination done");
    break;
  default:
    cs = ChargingStatus::Unknown;
    ESP_LOGI(TAG, "Charging status: unknown");
    break;
  }

  return cs;
}

bool BQ25XX::getVBATRaw(uint16_t *output) {
  if (!_readRegister(REG_VBAT_ADC, 2, output)) {
    ESP_LOGE(TAG, "Failed to get VBAT raw");
    return false;
  }
  return true;
}

bool BQ25XX::getVSYSRaw(uint16_t *output) {
  if (!_readRegister(REG_VSYS_ADC, 2, output)) {
    ESP_LOGE(TAG, "Failed to get VSYS raw");
    return false;
  }
  return true;
}

bool BQ25XX::getTemperatureRaw(uint16_t *output) {
  if (!_readRegister(REG_TEMP_ADC, 2, output)) {
    ESP_LOGE(TAG, "Failed to get temperature raw");
    return false;
  }
  return true;
}

bool BQ25XX::getVBUSRaw(uint16_t *output) {
  // First try reading from ADC1
  uint16_t adc1_value = 0;
  if (!_readRegister(REG_VBUS_ADC1, 2, &adc1_value)) {
    ESP_LOGE(TAG, "Failed to get VBUS ADC1");
    return false;
  }

  // If ADC1 value is >= threshold, use it
  if (adc1_value >= VBUS_ADC1_THRESHOLD) {
    *output = adc1_value;
    return true;
  }

  // Otherwise, fall back to ADC2
  if (!_readRegister(REG_VBUS_ADC2, 2, output)) {
    ESP_LOGE(TAG, "Failed to get VBUS ADC2");
    return false;
  }

  return true;
}

bool BQ25XX::getIBATRaw(uint16_t *output) {
  if (!_readRegister(REG_IBAT_ADC, 2, output)) {
    ESP_LOGE(TAG, "Failed to get IBAT raw");
    return false;
  }
  return true;
}

void BQ25XX::printSystemStatus() {
  ESP_LOGI(TAG, "==== SYSTEM STATUS ====");
  uint16_t value = 0;

  if (_readRegister(REG_SYSTEM_STATUS, 1, &value)) {
    ESP_LOGI(TAG, "System status (0x%.2x): 0x%.2x", REG_SYSTEM_STATUS,
             (uint8_t)value);
  }
  if (_readRegister(REG_FAULT_STATUS, 1, &value)) {
    ESP_LOGI(TAG, "Fault status (0x%.2x): 0x%.2x", REG_FAULT_STATUS,
             (uint8_t)value);
  }
  if (_readRegister(REG_CHARGE_STATUS, 1, &value)) {
    ESP_LOGI(TAG, "Charge status (0x%.2x): 0x%.2x", REG_CHARGE_STATUS,
             (uint8_t)value);
  }
  if (_readRegister(REG_INPUT_STATUS, 1, &value)) {
    ESP_LOGI(TAG, "Input status (0x%.2x): 0x%.2x", REG_INPUT_STATUS,
             (uint8_t)value);
  }
  if (_readRegister(REG_MPPT, 1, &value)) {
    ESP_LOGI(TAG, "MPPT status (0x%.2x): 0x%.2x", REG_MPPT, (uint8_t)value);
  }
  ESP_LOGI(TAG, "*********");
}

void BQ25XX::printControlAndConfiguration() {
  ESP_LOGI(TAG, "==== CONTROL and CONFIGURATION REGISTERS ====");
  uint16_t value = 0;

  if (_readRegister(REG00_MINIMAL_SYSTEM_VOLTAGE, 1, &value)) {
    ESP_LOGI(TAG,
             "Minimal System Voltage Raw (0x%.2X): 0x%04X | Converted: %d mV",
             REG00_MINIMAL_SYSTEM_VOLTAGE, value, ((value * 250) + 2500));
  }
  if (_readRegister(REG01_CHARGE_VOLTAGE_LIMIT, 2, &value)) {
    ESP_LOGI(TAG,
             "Charge Voltage Limit Raw (0x%.2X): 0x%04X | Converted: %d mV",
             REG01_CHARGE_VOLTAGE_LIMIT, value, (value * 10));
  }
  if (_readRegister(REG03_CHARGE_CURRENT_LIMIT, 2, &value)) {
    ESP_LOGI(TAG,
             "Charge Current Limit Raw (0x%.2X): 0x%04X | Converted: %d mA",
             REG03_CHARGE_CURRENT_LIMIT, value, (value * 10));
  }
  if (_readRegister(REG05_INPUT_VOLTAGE_LIMIT, 1, &value)) {
    ESP_LOGI(TAG, "Input Voltage Limit Raw (0x%.2X): 0x%04X | Converted: %d mV",
             REG05_INPUT_VOLTAGE_LIMIT, value, (value * 100));
  }
  if (_readRegister(REG06_INPUT_CURRENT_LIMIT, 2, &value)) {
    ESP_LOGI(TAG, "Input Current Limit Raw (0x%.2X): 0x%04X | Converted: %d mA",
             REG06_INPUT_CURRENT_LIMIT, value, (value * 10));
  }
  if (_readRegister(REG_ADC_CTRL, 1, &value)) {
    ESP_LOGI(TAG, "ADC Control (0x%.2X): 0x%02X", REG_ADC_CTRL, (uint8_t)value);
  }
  ESP_LOGI(TAG, "*********");
}

bool BQ25XX::_writeRegister(uint8_t reg, uint8_t value) {
  uint8_t txBuf[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(_devHandle, txBuf, 2, 500);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write register 0x%.2x: %s", reg,
             esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ25XX::_readRegister(uint8_t reg, uint8_t size, uint16_t *output) {
  if (size < 1 || size > 2) {
    ESP_LOGE(TAG, "Invalid register size: %d", size);
    return false;
  }

  uint8_t buffer[2] = {0};
  esp_err_t err =
      i2c_master_transmit_receive(_devHandle, &reg, 1, buffer, size, 500);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read register 0x%.2x: %s", reg,
             esp_err_to_name(err));
    return false;
  }

  // Compile result (big-endian)
  uint16_t value = 0;
  for (uint8_t i = 0; i < size; i++) {
    value = (value << 8) | buffer[i];
  }
  *output = value;

  return true;
}
