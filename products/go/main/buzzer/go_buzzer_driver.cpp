/**
 * AirGradient Go -- LEDC buzzer driver implementation
 *
 * Drives the HYG-8503A magnetic buzzer through LEDC PWM via an NPN
 * low-side switch.  Contains all ESP-IDF LEDC dependencies; not linked
 * into host tests.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_buzzer_driver.h"

#include "ag_log.h"

static constexpr const char *TAG = "LedcBuzzer";

// ---------------------------------------------------------------------------
// LEDC configuration constants
// ---------------------------------------------------------------------------

static constexpr uint32_t LEDC_DUTY_RESOLUTION_BITS = 10;
static constexpr uint32_t LEDC_DUTY_MAX = (1u << LEDC_DUTY_RESOLUTION_BITS); // 1024
static constexpr uint32_t LEDC_MIN_DUTY = 1; ///< Minimum audible edge

// ===========================================================================
// Construction
// ===========================================================================

LedcBuzzer::LedcBuzzer(const Config &config) : _config(config) {}

// ===========================================================================
// Public interface
// ===========================================================================

bool LedcBuzzer::init() {
#ifndef TEST_HOST
  // 1. Configure LEDC timer
  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_cfg.timer_num = static_cast<ledc_timer_t>(_config.ledc_timer);
  timer_cfg.duty_resolution = static_cast<ledc_timer_bit_t>(LEDC_DUTY_RESOLUTION_BITS);
  timer_cfg.freq_hz = _config.default_freq_hz;
  timer_cfg.clk_cfg = LEDC_AUTO_CLK;

  if (ledc_timer_config(&timer_cfg) != ESP_OK) {
    AG_LOGE(TAG, "timer config failed");
    return false;
  }

  // 2. Configure LEDC channel with duty 0 (muted)
  ledc_channel_config_t ch_cfg = {};
  ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  ch_cfg.channel = static_cast<ledc_channel_t>(_config.ledc_channel);
  ch_cfg.timer_sel = static_cast<ledc_timer_t>(_config.ledc_timer);
  ch_cfg.intr_type = LEDC_INTR_DISABLE;
  ch_cfg.gpio_num = _config.pin;
  ch_cfg.duty = 0;
  ch_cfg.hpoint = 0;

  if (ledc_channel_config(&ch_cfg) != ESP_OK) {
    AG_LOGE(TAG, "channel config failed");
    return false;
  }

  // 3. Mark ready
  _ledc_ready = true;
  AG_LOGI(TAG, "init ok (pin %d, freq %lu Hz)", _config.pin,
          static_cast<unsigned long>(_config.default_freq_hz));
  return true;
#else
  _ledc_ready = true;
  return true;
#endif
}

bool LedcBuzzer::set_freq(uint32_t freq_hz) {
  if (!_ledc_ready) {
    return false;
  }

#ifndef TEST_HOST
  auto mode = LEDC_LOW_SPEED_MODE;
  auto channel = static_cast<ledc_channel_t>(_config.ledc_channel);
  auto timer = static_cast<ledc_timer_t>(_config.ledc_timer);

  if (freq_hz == 0) {
    // Mute: set duty to 0
    if (ledc_set_duty(mode, channel, 0) != ESP_OK) {
      return false;
    }
    return ledc_update_duty(mode, channel) == ESP_OK;
  }

  // Set timer frequency
  if (ledc_set_freq(mode, timer, freq_hz) != ESP_OK) {
    return false;
  }

  // Compute duty from duty_percent, enforce minimum of 1
  uint32_t duty = (LEDC_DUTY_MAX * _config.duty_percent) / 100;
  if (duty < LEDC_MIN_DUTY) {
    duty = LEDC_MIN_DUTY;
  }

  if (ledc_set_duty(mode, channel, duty) != ESP_OK) {
    return false;
  }
  return ledc_update_duty(mode, channel) == ESP_OK;
#else
  (void)freq_hz;
  return true;
#endif
}
