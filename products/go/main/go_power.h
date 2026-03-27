/**
 * AirGradient Go — Power Management Service
 *
 * Handles BMS status polling, battery monitoring, sleep cycle management,
 * RTC state persistence, and shutdown.  Called synchronously by the
 * orchestrator — no independent task.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "airgradient_gpio.h"
#include "go_settings.h"
#include "go_types.h"
#include "hal/bms_device.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// PowerSnapshot
// ---------------------------------------------------------------------------

/// Aggregated BMS snapshot for the orchestrator and display.
/// All fields are initialized to invalid sentinels; call poll_bms() to fill.
struct PowerSnapshot {
  float battery_voltage = BmsInvalid::VOLT;
  float charging_voltage = BmsInvalid::VOLT;
  float battery_percentage = -1.0f;
  BmsChargingState charging_status = BmsChargingState::Unknown;
  bool critical = false; ///< true when battery_percentage < BATTERY_CRITICAL_PERCENT
};

// ---------------------------------------------------------------------------
// PowerService
// ---------------------------------------------------------------------------

class PowerService {
public:
  // -------------------------------------------------------------------------
  // Configuration
  // -------------------------------------------------------------------------

  struct Config {
    int pin_wake_button_power;          ///< GPIO for deep sleep wake (Button Power)
    int pin_wake_button_boot;           ///< GPIO for deep sleep wake (Button Boot)
    int pin_ext_wdt = -1;               ///< External watchdog GPIO (-1 = disabled)
    int deep_sleep_threshold_ms = 5000; ///< Minimum interval (ms) to prefer deep sleep
  };

  // -------------------------------------------------------------------------
  // Sleep type
  // -------------------------------------------------------------------------

  /// Sleep type selected by decide_sleep().
  enum class SleepType {
    None,  ///< Do not sleep (device is unlocked or not Offline)
    Light, ///< Light sleep — CPU paused, peripherals active; returns on wake
    Deep,  ///< Deep sleep — CPU reboots on wake; does not return from enter_sleep()
  };

  /// Combined result of sleep decision: what type and for how long.
  struct SleepDecision {
    SleepType type;       ///< None, Light, or Deep
    uint32_t duration_ms; ///< How long to sleep (0 when type == None)
  };

  // -------------------------------------------------------------------------
  // Construction
  // -------------------------------------------------------------------------

  /// @param bms     BMS device (via BmsDevice HAL).  Must outlive this service.
  /// @param gpio    GPIO HAL function-pointer table.
  /// @param config  Runtime configuration (wake pins, sleep threshold).
  PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config);

  // -------------------------------------------------------------------------
  // BMS operations (called by orchestrator on timer)
  // -------------------------------------------------------------------------

  /// Poll BMS for current status.  Fast I2C read, non-blocking.
  /// Returns a PowerSnapshot with all fields populated (invalid sentinels on error).
  PowerSnapshot poll_bms();

  /// Reset BMS watchdog.  Must be called periodically (< 10 s interval).
  /// @return true if the watchdog reset succeeded.
  bool reset_watchdog();

  /// Trigger BMS QoN (ship mode).  Device powers off.  Does not return.
  void shutdown();

  // -------------------------------------------------------------------------
  // External watchdog
  // -------------------------------------------------------------------------

  /// Configure the external watchdog GPIO as output (LOW).
  /// No-op if pin_ext_wdt < 0 in Config.
  void init_ext_watchdog();

  /// Pulse the external watchdog GPIO (HIGH 20 ms, then LOW).
  /// No-op if pin_ext_wdt < 0 in Config.
  void reset_ext_watchdog();

  // -------------------------------------------------------------------------
  // RTC state persistence
  // -------------------------------------------------------------------------

  /// Save application state to RTC memory before sleep.
  /// Under TEST_HOST the state is stored in a regular static variable.
  void save_state(const RtcAppState &state);

  /// Load application state from RTC memory after wake.
  /// Returns default-constructed RtcAppState when no valid state has been saved.
  RtcAppState load_state() const;

  // -------------------------------------------------------------------------
  // Sleep cycle
  // -------------------------------------------------------------------------

  /// Determine whether to sleep, which type, and for how long.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  /// Uses Config::deep_sleep_threshold_ms from construction.
  ///
  /// Rules:
  ///   - Not Offline mode  -> {None, 0}  (only Offline sleeps)
  ///   - Unlocked          -> {None, 0}  (never sleep while user is active)
  ///   - sleep_ms >= deep_sleep_threshold_ms -> {Deep, sleep_ms}
  ///   - sleep_ms <  deep_sleep_threshold_ms -> {Light, sleep_ms}
  ///
  /// sleep_ms = min(enabled intervals) - awake_ms, clamped to 0.
  ///
  /// @param settings   Current settings (sensor and display intervals).
  /// @param lock_state Current lock state.
  /// @param mode       Current operating mode.
  /// @param awake_ms   Milliseconds the device has been awake this cycle.
  SleepDecision decide_sleep(const GoSettings &settings, LockState lock_state, OperatingMode mode,
                             uint32_t awake_ms) const;

  /// Enter the requested sleep type.
  ///
  /// Deep sleep: configures wake sources and calls esp_deep_sleep_start().
  ///   Does not return — CPU reboots on wake.
  ///
  /// Light sleep: configures wake sources, calls esp_light_sleep_start(),
  ///   and returns the wake cause after the CPU resumes.
  ///
  /// @param type              Sleep type from decide_sleep().
  /// @param sleep_duration_ms How long to sleep before timer wake.
  /// @return Wake cause (only meaningful for Light sleep; Deep never returns).
  WakeCause enter_sleep(SleepType type, uint32_t sleep_duration_ms);

  // -------------------------------------------------------------------------
  // Boot path (static — call before any service is constructed)
  // -------------------------------------------------------------------------

  /// Determine wake cause early in app_main.
  /// Translates esp_sleep_get_wakeup_cause() to WakeCause.
  static WakeCause get_wake_cause();

  /// Returns true when this boot should follow the abbreviated fast path:
  ///   cause == WakeCause::Timer && state.lock_state == LockState::Locked
  ///
  /// Pure logic — no platform dependencies; testable on host.
  static bool is_fast_path_wake(WakeCause cause, const RtcAppState &state);

  // -------------------------------------------------------------------------
  // Constants
  // -------------------------------------------------------------------------

  /// Battery percentage below which the critical flag is set in PowerSnapshot.
  /// Fixed threshold — not a user-configurable setting.
  static constexpr float BATTERY_CRITICAL_PERCENT = 5.0f;

private:
  BmsDevice &_bms;
  const gpio::Hal &_gpio;
  Config _config;

  /// Configure timer and GPIO wake sources before entering sleep.
  /// Wrapped in #ifndef TEST_HOST — not callable from host test builds.
  void configure_wake_sources(uint32_t timer_ms);
};

// ---------------------------------------------------------------------------
// Free function — early boot path
// ---------------------------------------------------------------------------

/// Read RtcAppState from RTC memory.  Returns default state if no valid
/// state has been saved.  No dependencies — safe to call early in app_main
/// before PowerService is constructed.
RtcAppState load_rtc_app_state();
