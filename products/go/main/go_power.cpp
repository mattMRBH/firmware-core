/**
 * AirGradient Go — Power Management Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

// ---------------------------------------------------------------------------
// Platform compatibility — RTC_DATA_ATTR
//
// On ESP-IDF, RTC_DATA_ATTR places a variable in RTC slow memory so it
// survives deep sleep.  In host test builds the attribute does not exist;
// define it away so the translation unit compiles as a regular static.
// ---------------------------------------------------------------------------

#if !defined(TEST_HOST) && defined(__has_include)
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#endif

#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif

// ---------------------------------------------------------------------------
// Platform compatibility — ESP-IDF sleep APIs
// ---------------------------------------------------------------------------

#ifndef TEST_HOST
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "go_power.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "ag_log.h"
#include "common.h"

static constexpr const char *TAG = "PowerService";

// ---------------------------------------------------------------------------
// RTC state storage
//
// These two variables live in RTC slow memory (survives deep sleep).
// Under TEST_HOST the RTC_DATA_ATTR is defined away, making them ordinary
// file-scope statics with the same semantics for unit testing.
// ---------------------------------------------------------------------------

RTC_DATA_ATTR static RtcAppState s_rtc_state;
RTC_DATA_ATTR static bool s_rtc_state_valid = false;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PowerService::PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config)
    : _bms(bms), _gpio(gpio), _config(config) {}

// ---------------------------------------------------------------------------
// BMS operations
// ---------------------------------------------------------------------------

PowerSnapshot PowerService::poll_bms() {
  PowerSnapshot status{};

  BmsTelemetry telemetry{};
  if (_bms.read_telemetry(telemetry)) {
    if (telemetry.is_battery_voltage_valid()) {
      status.battery_voltage = telemetry.battery_voltage;
    }
    if (telemetry.is_charging_voltage_valid()) {
      status.charging_voltage = telemetry.charging_voltage;
    }
  } else {
    AG_LOGW(TAG, "poll_bms: read_telemetry() failed");
  }

  float pct = -1.0f;
  if (_bms.get_battery_percentage(&pct)) {
    status.battery_percentage = pct;
    status.critical = (pct >= 0.0f && pct < BATTERY_CRITICAL_PERCENT);
  } else {
    AG_LOGW(TAG, "poll_bms: get_battery_percentage() failed");
  }

  BmsStatus bms_status{};
  if (_bms.read_status(bms_status)) {
    status.charging_status = bms_status.charging_state;
  }

  return status;
}

bool PowerService::reset_watchdog() {
  const bool ok = _bms.update_watchdog();
  if (!ok) {
    AG_LOGW(TAG, "reset_watchdog: update_watchdog() failed");
  }
  return ok;
}

void PowerService::shutdown() {
#ifndef TEST_HOST
  AG_LOGI(TAG, "shutdown: entering BMS ship mode (QoN)");
  if (!_bms.enter_ship_mode()) {
    AG_LOGE(TAG, "shutdown: enter_ship_mode failed — spinning");
  }
  // enter_ship_mode() cuts system power and should not return.
  // If it does (error or unsupported), spin to preserve the "does not return"
  // contract.
  // TODO: Better to use esp_deep_sleep rather then block loop
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif
}

// ---------------------------------------------------------------------------
// External watchdog
// ---------------------------------------------------------------------------

void PowerService::init_ext_watchdog() {
  if (_config.pin_ext_wdt < 0) {
    return;
  }
  if (!ext_watchdog_init(_gpio, _config.pin_ext_wdt)) {
    AG_LOGE(TAG, "init_ext_watchdog: GPIO %d config failed", _config.pin_ext_wdt);
  }
}

void PowerService::reset_ext_watchdog() {
  if (_config.pin_ext_wdt < 0) {
    return;
  }
  ext_watchdog_reset(_gpio, _config.pin_ext_wdt);
}

// ---------------------------------------------------------------------------
// RTC state persistence
// ---------------------------------------------------------------------------

void PowerService::save_state(const RtcAppState &state) {
  memcpy(&s_rtc_state, &state, sizeof(RtcAppState));
  s_rtc_state_valid = true;
}

RtcAppState PowerService::load_state() const {
  if (!s_rtc_state_valid) {
    // Return defaults: Offline mode, Idle behavior, Locked, GPS on.
    return RtcAppState{};
  }
  RtcAppState out{};
  memcpy(&out, &s_rtc_state, sizeof(RtcAppState));
  return out;
}

// ---------------------------------------------------------------------------
// decide_sleep — pure logic (no platform dependencies)
// ---------------------------------------------------------------------------

PowerService::SleepDecision PowerService::decide_sleep(const GoSettings &settings,
                                                       LockState lock_state, OperatingMode mode,
                                                       uint32_t awake_ms) const {
  // Only Offline mode enters sleep; Portable and Stationary stay awake.
  if (mode != OperatingMode::Offline) {
    return {SleepType::None, 0};
  }

  if (lock_state == LockState::Unlocked) {
    return {SleepType::None, 0};
  }

  // Determine the minimum enabled interval.
  uint32_t interval_ms = UINT32_MAX;

  if (settings.pm_interval_seconds > 0) {
    interval_ms = std::min(interval_ms, static_cast<uint32_t>(settings.pm_interval_seconds) * 1000);
  }

  if (settings.other_sensor_interval_seconds > 0) {
    interval_ms =
        std::min(interval_ms, static_cast<uint32_t>(settings.other_sensor_interval_seconds) * 1000);
  }

  if (settings.display_refresh_interval_seconds > 0) {
    interval_ms = std::min(interval_ms,
                           static_cast<uint32_t>(settings.display_refresh_interval_seconds) * 1000);
  }

  // Fallback if everything is disabled
  if (interval_ms == UINT32_MAX) {
    interval_ms = 60000;
  }

  // Subtract time already spent awake so total cycle matches the interval
  uint32_t sleep_ms = (awake_ms < interval_ms) ? (interval_ms - awake_ms) : 0;

  if (sleep_ms >= static_cast<uint32_t>(_config.deep_sleep_threshold_ms)) {
    return {SleepType::Deep, sleep_ms};
  }
  // Interval too short: deep sleep overhead (~3–4 s reboot) exceeds the
  // sleep duration.  Stay awake and let the main loop run normally.
  return {SleepType::None, 0};
}

// ---------------------------------------------------------------------------
// Sleep entry — platform-specific, guarded by #ifndef TEST_HOST
// ---------------------------------------------------------------------------

void PowerService::enter_sleep(uint32_t sleep_duration_ms) {
#ifndef TEST_HOST
  AG_LOGI(TAG, "enter_sleep: entering deep sleep for %" PRIu32 " ms", sleep_duration_ms);
  configure_wake_sources(sleep_duration_ms);
  esp_deep_sleep_start();
  // Does not return — CPU reboots on wake.
#endif
}

// ---------------------------------------------------------------------------
// Wake source configuration — platform-specific, guarded by #ifndef TEST_HOST
// ---------------------------------------------------------------------------

void PowerService::configure_wake_sources(uint32_t timer_ms) {
#ifndef TEST_HOST
  // Timer wake: convert milliseconds to microseconds for the ESP-IDF API.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_ms) * 1000ULL);

  // GPIO wake: both buttons use EXT1 which supports multiple GPIOs in a
  // single bitmask.  The target (ESP32-C5) does not have EXT0; EXT1 is the
  // correct deep-sleep GPIO wake source.  Buttons are active-low, so wake
  // fires when ANY selected GPIO is pulled low.
  uint64_t wake_mask = 0;
  if (_config.pin_wake_button_power >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_power;
  }
  if (_config.pin_wake_button_boot >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_boot;
  }
  if (wake_mask != 0) {
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
#endif
}

// ---------------------------------------------------------------------------
// Boot path — static helpers
// ---------------------------------------------------------------------------

// static
WakeCause PowerService::get_wake_cause() {
#ifndef TEST_HOST
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  switch (cause) {
  case ESP_SLEEP_WAKEUP_TIMER:
    return WakeCause::Timer;
  case ESP_SLEEP_WAKEUP_EXT0:
  case ESP_SLEEP_WAKEUP_EXT1:
  case ESP_SLEEP_WAKEUP_GPIO:
    return WakeCause::Button;
  default:
    // ESP_SLEEP_WAKEUP_UNDEFINED = first power-on (not a wake from sleep).
    return WakeCause::PowerOn;
  }
#else
  return WakeCause::PowerOn;
#endif
}

// static
bool PowerService::is_fast_path_wake(WakeCause cause, const RtcAppState &state) {
  return cause == WakeCause::Timer && state.lock_state == LockState::Locked;
}

// ---------------------------------------------------------------------------
// Free function — early boot path
// ---------------------------------------------------------------------------

RtcAppState load_rtc_app_state() {
  if (!s_rtc_state_valid) {
    return RtcAppState{};
  }
  RtcAppState out{};
  memcpy(&out, &s_rtc_state, sizeof(RtcAppState));
  return out;
}
