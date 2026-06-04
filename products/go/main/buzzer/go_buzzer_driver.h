/**
 * AirGradient Go -- LEDC buzzer driver declaration
 *
 * Concrete BuzzerDriver implementation for the HYG-8503A magnetic buzzer
 * driven via ESP-IDF LEDC PWM.  Production wiring includes this header;
 * BuzzerService does not.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_buzzer_hal.h"

#include <cstdint>

#ifndef TEST_HOST
#include "driver/ledc.h"
#endif

class LedcBuzzer final : public BuzzerDriver {
public:
  struct Config {
    int pin = -1;
    uint32_t default_freq_hz = 2700;
    uint8_t duty_percent = 50;
    uint8_t ledc_channel = 0;
    uint8_t ledc_timer = 0;
  };

  explicit LedcBuzzer(const Config &config);
  ~LedcBuzzer() override = default;

  LedcBuzzer(const LedcBuzzer &) = delete;
  LedcBuzzer &operator=(const LedcBuzzer &) = delete;

  bool init() override;
  bool set_freq(uint32_t freq_hz) override;

private:
  Config _config;
  bool _ledc_ready = false;
};
