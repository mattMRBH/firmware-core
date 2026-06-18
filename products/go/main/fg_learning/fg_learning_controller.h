/**
 * AirGradient Go — Fuel-Gauge Learning Controller (pure FSM)
 *
 * Pure decision core for the factory fuel-gauge learning run. Reads a
 * PowerSnapshot and emits an FgLearningAction; never touches hardware or
 * ESP-IDF, so it is host-testable. Owned and ticked by FgLearningRunner.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "go_display.h"  // Screen
#include "go_power.h"    // PowerSnapshot
#include "go_settings.h" // FgLearningStage

// Pure RA-grid size for the FSM/verify layer. Declared here (not pulled from
// bq27427.h) so the controller stays free of the driver header. Cross-checked
// against the shared bms_types.h constant so the two cannot drift.
static constexpr size_t FG_LEARNING_RA_TABLE_SIZE = 15;
static_assert(FG_LEARNING_RA_TABLE_SIZE == FG_RA_TABLE_SIZE,
              "FG learning Ra table size must match the gauge driver");

// Static-LED indicator for stages that need operator action. The runner maps
// each to one solid back-LED colour. Kept on the action so the FSM stays pure.
enum class ManualCue : uint8_t { None, Unplug, Failed, Complete };

struct FgLearningAction {
  bool set_charge_enabled = false;        ///< desired BMS charge-enable state
  uint16_t charge_current_ma = 0;         ///< ICHG to program while charging
  bool low_power = false;                 ///< quiet load (Rest/Verify) vs full load
  ManualCue manual_cue = ManualCue::None; ///< solid LED for operator-action stages
  Screen screen = Screen::Home;           ///< phase screen to paint
  bool persist_stage = false;             ///< write factory settings now
  bool run_verify = false;                ///< runner reads FG + calls on_verify_result()
  bool active = false;                    ///< false in {Idle, Complete, Failed}
};
// Note: no commit_then_ship field. The EDV persist-then-ship ordering is owned
// by FgLearningRunner::handle_edv_ship(); the FSM only transitions
// Discharge -> CycleDone on edv_cutoff_reached.

struct VerifyInputs {
  bool reads_ok = false;
  bool itpor = false;    ///< a POR wiped learning
  bool qmax_up = false;  ///< CONTROL_STATUS QMAX_UP
  uint16_t qmax_mah = 0; ///< learned Qmax converted to mAh
  uint16_t design_capacity_mah = 0;
  int16_t ra[FG_LEARNING_RA_TABLE_SIZE] = {};
};

class FgLearningController {
public:
  static constexpr uint8_t CYCLE_TARGET = 2;          ///< fixed cycles before verify
  static constexpr uint8_t ITPOR_LOSS_CAP = 3;        ///< POR-loss restarts -> Failed
  static constexpr uint32_t REST_TIMEOUT_MS = 500000; ///< min rest before discharge
  static constexpr uint32_t CHARGE_TIMEOUT_MS = 8u * 60u * 60u * 1000u;
  static constexpr uint16_t CHARGE_CURRENT_MA = 1500;

  // Qmax accepted band, as a fraction of design capacity (verify criterion 3).
  static constexpr float QMAX_MIN_FRACTION = 0.7f;
  static constexpr float QMAX_MAX_FRACTION = 1.4f;

  void load(FgLearningStage stage, uint8_t cycle, uint8_t itpor_losses);
  void start(); ///< arm a fresh run (stage=Charge, cycle=1)
  void reset(); ///< clear to Idle
  bool resume_on_boot(const PowerSnapshot &snap);
  FgLearningAction tick(const PowerSnapshot &snap, uint32_t now_ms);
  bool on_verify_result(const VerifyInputs &in);
  static bool verify_pass(const VerifyInputs &in); ///< pure, host-tested

  FgLearningStage stage() const { return _stage; }
  uint8_t cycle() const { return _cycle; }
  uint8_t itpor_losses() const { return _itpor_losses; }

private:
  // Move into a stage: records the entry timestamp and resets per-stage state.
  void enter_stage(FgLearningStage stage, uint32_t now_ms);
  // Cap-guarded POR-loss restart: bumps the loss count and either restarts
  // the current cycle's Charge or trips Failed at the cap.
  void por_loss_restart();
  // Derive the action for the current stage (without transition logic).
  FgLearningAction build_action() const;

  FgLearningStage _stage = FgLearningStage::Idle;
  uint8_t _cycle = 0;
  uint8_t _itpor_losses = 0;
  uint32_t _stage_entered_ms = 0;
  bool _stage_time_set = false;  ///< false until the first tick stamps entry time
  bool _charge_observed = false; ///< saw active charging during this Charge stage
};
