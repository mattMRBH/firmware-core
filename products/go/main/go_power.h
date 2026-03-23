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
    int deep_sleep_threshold_ms = 5000; ///< Minimum interval (ms) to prefer deep sleep
  };

  // -------------------------------------------------------------------------
  // Sleep type
  // -------------------------------------------------------------------------

  /// Sleep type selected by evaluate_sleep().
  enum class SleepType {
    None,  ///< Do not sleep (device is unlocked)
    Light, ///< Light sleep — CPU paused, peripherals active; returns on wake
    Deep,  ///< Deep sleep — CPU reboots on wake; does not return from enter_sleep()
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
  ///
  /// @note The BmsDevice HAL exposes enter_ship_mode() but no concrete
  ///       driver implements it yet.  This is a stub that logs the intent
  ///       and spins until hardware support is added to the driver.
  /// @todo Implement once a BmsDevice driver supports ship mode.
  void shutdown();

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

  /// Determine the appropriate sleep type for the next wake interval.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  ///
  /// Rules:
  ///   - Not Offline mode  -> SleepType::None  (only Offline sleeps)
  ///   - Unlocked   -> SleepType::None  (never sleep while user is active)
  ///   - next_wake_ms >= deep_sleep_threshold_ms -> SleepType::Deep
  ///   - next_wake_ms <  deep_sleep_threshold_ms -> SleepType::Light
  SleepType evaluate_sleep(const GoSettings &settings, LockState lock_state,
                           OperatingMode mode) const;

  /// Enter the requested sleep type.
  ///
  /// Deep sleep: configures wake sources and calls esp_deep_sleep_start().
  ///   Does not return — CPU reboots on wake.
  ///
  /// Light sleep: configures wake sources, calls esp_light_sleep_start(),
  ///   and returns the wake cause after the CPU resumes.
  ///
  /// @param type              Sleep type from evaluate_sleep().
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
