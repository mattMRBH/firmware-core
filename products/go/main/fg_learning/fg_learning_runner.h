/**
 * AirGradient Go — Fuel-Gauge Learning Runner (hardware-owning)
 *
 * Dedicated factory boot path that drives the whole multi-hour, multi-cycle
 * learning sequence in isolation: it owns the pure FgLearningController, brings
 * up only the hardware the run needs (display, LED, buzzer), and never
 * constructs the Orchestrator or any normal producer. Target-only — never
 * host-compiled.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

#include "fg_learning/fg_learning_controller.h"
#include "go_power.h"

class GoBoard;
class DisplayService;
class LedService;
class BuzzerService;
class ConfigStore;

class FgLearningRunner {
public:
  struct Deps {
    PowerService &power;       ///< poll, charge/load ctl, verify read, shutdown, ext WDT
    DisplayService &display;   ///< dashboard
    LedService &led;           ///< manual-cue LED
    BuzzerService &buzzer;     ///< unplug melody
    ConfigStore &config_store; ///< FactorySettings load / save / clear
    GoBoard &board;            ///< gpio_hal (abort button); reboot is a free fn
  };

  explicit FgLearningRunner(const Deps &deps);

  /// Bring-up + resume + run loop. Never returns.
  [[noreturn]] void run();

private:
  void resume_on_boot();                               ///< load -> controller, prereqs, one poll
  void apply_action(const FgLearningAction &a);        ///< charge / load / cue / screen / persist
  void run_verify();                                   ///< read FG, on_verify_result, persist
  bool handle_edv_ship(const PowerSnapshot &snap);     ///< persist CycleDone -> ship (single owner)
  void feed_ext_watchdog(uint32_t now);                ///< pulse external HW WDT (< 60 s window)
  void refresh_dashboard(const PowerSnapshot &snap);   ///< build DisplayValues, full refresh
  bool poll_abort_button();                            ///< boot long-press -> clear + reboot
  void idle_poll(uint32_t total_ms, bool watch_abort); ///< responsive delay; samples abort button
  [[noreturn]] void handback_terminal();               ///< cleanup + result paint/LED, hold

  void set_discharge_load(bool on); ///< PM rail + CPU duty flag
  void run_cpu_duty();              ///< deliberate busy fraction during Discharge
  DisplayValues build_dashboard_values(const PowerSnapshot &snap) const;
  void terminal_cleanup(); ///< idempotent restore for both terminal stages

  FgLearningController _controller; ///< pure FSM, owned
  Deps _deps;

  uint32_t _last_paint_ms = 0;
  uint32_t _last_wdt_ms = 0;
  uint32_t _stage_entered_ms = 0; ///< wall-clock when the current stage was entered (this boot)
  uint32_t _boot_btn_down_ms = 0; ///< 0 = not pressed; else press-start time
  bool _abort_btn_ready = false;  ///< boot button GPIO configured
  bool _discharge_load_on = false;
  bool _pm_fan_running = false;             ///< PM fan confirmed measuring (valid read)
  Screen _screen = Screen::FgLearnCharging; ///< latest phase screen for the dashboard
  FgLearningStage _prev_stage = FgLearningStage::Idle;
  bool _prev_stage_valid = false;

  // Repaint promptly on a charging-state / plug change (not just the heartbeat).
  BmsChargingState _prev_charging_status = BmsChargingState::Unknown;
  bool _prev_external_input_present = false;
  bool _prev_inputs_valid = false;
};
