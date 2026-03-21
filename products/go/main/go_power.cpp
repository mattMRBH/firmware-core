/**
 * AirGradient Go — Power Management Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

// ---------------------------------------------------------------------------
// Platform compatibility — ESP-IDF logging
// ---------------------------------------------------------------------------

#ifdef TEST_HOST
#define ESP_LOGI(tag, ...)                                                                         \
  do {                                                                                             \
  } while (0)
#define ESP_LOGW(tag, ...)                                                                         \
  do {                                                                                             \
  } while (0)
#define ESP_LOGE(tag, ...)                                                                         \
  do {                                                                                             \
  } while (0)
#else
#include "esp_log.h"
#endif

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
#endif

#include "go_power.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

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

PowerService::PowerService(BQ25XX &bms, const gpio::Hal &gpio, const Config &config)
    : _bms(bms), _gpio(gpio), _config(config) {}

// ---------------------------------------------------------------------------
// BMS operations
// ---------------------------------------------------------------------------

BmsStatus PowerService::poll_bms() {
  BmsStatus status{};

  BatteryMgmtData batt_data{};
  batt_data.volt_battery = MeasuresInvalid::VOLT;
  batt_data.volt_charging = MeasuresInvalid::VOLT;

  if (_bms.read(batt_data)) {
    if (batt_data.is_vbat_valid()) {
      status.battery_voltage = batt_data.volt_battery;
    }
    if (batt_data.is_vpanel_valid()) {
      status.charging_voltage = batt_data.volt_charging;
    }
  } else {
    ESP_LOGW(TAG, "poll_bms: BatteryMgmtSensor::read() failed");
  }

  float pct = -1.0f;
  if (_bms.getBatteryPercentage(&pct)) {
    status.battery_percentage = pct;
    status.critical = (pct >= 0.0f && pct < BATTERY_CRITICAL_PERCENT);
  } else {
    ESP_LOGW(TAG, "poll_bms: getBatteryPercentage() failed");
  }

  status.charging_status = _bms.getChargingStatus();

  return status;
}

bool PowerService::reset_watchdog() {
  const bool ok = _bms.updateWatchdog();
  if (!ok) {
    ESP_LOGW(TAG, "reset_watchdog: updateWatchdog() failed");
  }
  return ok;
}

void PowerService::shutdown() {
  // TODO: Implement QoN / ship-mode shutdown once BQ25XX::enterShipMode()
  // (or an equivalent method) is added to the BQ25XX driver.
  //
  // QoN is triggered by writing specific register values that put the BMS
  // into ship mode, cutting power to the entire system.  The expected call
  // sequence once the driver method exists is:
  //
  //   _bms.enterShipMode();   // writes QoN registers; this call does not
  //                            // return because the system loses power
  //
  // See: components/airgradient-sensors/drivers/bq25xx/bq25xx.h
  //      BQ25672 / BQ25798 datasheet — Ship Mode / QoN register sequence.
  //
  // Until then, log the intent and spin so the caller's "does not return"
  // contract is preserved at the cost of the battery draining.
#ifndef TEST_HOST
  ESP_LOGI(TAG, "shutdown: QoN stub — BQ25XX ship-mode not yet implemented; spinning");
  while (true) {
    // Busy-wait until hardware support is wired in.
  }
#endif
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
// Sleep cycle — pure logic (no platform dependencies)
// ---------------------------------------------------------------------------

PowerService::SleepType PowerService::evaluate_sleep(const GoSettings &settings,
                                                     LockState lock_state) const {
  if (lock_state == LockState::Unlocked) {
    return SleepType::None;
  }

  // Determine the minimum interval until the next required wake action.
  int next_wake_ms = settings.measurement_interval_seconds * 1000;

  if (settings.display_refresh_interval_seconds > 0) {
    const int display_ms = settings.display_refresh_interval_seconds * 1000;
    next_wake_ms = std::min(next_wake_ms, display_ms);
  }

  if (next_wake_ms >= _config.deep_sleep_threshold_ms) {
    return SleepType::Deep;
  }
  return SleepType::Light;
}

// ---------------------------------------------------------------------------
// Sleep entry — platform-specific, guarded by #ifndef TEST_HOST
// ---------------------------------------------------------------------------

WakeCause PowerService::enter_sleep(SleepType type, uint32_t sleep_duration_ms) {
#ifndef TEST_HOST
  configure_wake_sources(sleep_duration_ms);

  if (type == SleepType::Deep) {
    ESP_LOGI(TAG, "enter_sleep: entering deep sleep for %" PRIu32 " ms", sleep_duration_ms);
    esp_deep_sleep_start();
    // Does not return — CPU reboots on wake.
  }

  if (type == SleepType::Light) {
    ESP_LOGI(TAG, "enter_sleep: entering light sleep for %" PRIu32 " ms", sleep_duration_ms);
    esp_light_sleep_start();
    // Execution resumes here after wake from light sleep.
    return get_wake_cause();
  }
#endif
  // SleepType::None, or TEST_HOST path — no sleep taken.
  return WakeCause::PowerOn;
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
