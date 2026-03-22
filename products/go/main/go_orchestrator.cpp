/**
 * AirGradient Go — Orchestrator implementation (stub)
 *
 * Minimal stub that receives events from the queue and logs them.
 * The full event dispatch, state machines, and consumer calls will be
 * added in a subsequent change.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_orchestrator.h"

#include <cstdint>

#include "ag_log.h"

#include "rtos.h"

static constexpr const char *TAG = "Orchestrator";

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Orchestrator::Orchestrator(RtosQueueHandle event_queue, Services &services, GoSettings &settings,
                           ConfigStore &config_store)
    : _event_queue(event_queue), _services(services), _settings(settings),
      _config_store(config_store) {}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void Orchestrator::init(WakeCause cause) {
  AG_LOGI(TAG, "init: wake_cause=%d", static_cast<int>(cause));

  // TODO: Restore app state from RTC for button wake.
  // TODO: Sync UI manager with current settings.
  // TODO: Start measurement and inactivity timers.
}

// ---------------------------------------------------------------------------
// Main event loop (stub)
// ---------------------------------------------------------------------------

void Orchestrator::run() {
  AG_LOGI(TAG, "run: entering main event loop");

  Event evt{};
  while (true) {
    if (RTOS::queue_receive(_event_queue, &evt, 1000)) {
      // TODO: Dispatch event to state machines and consumers.
      AG_LOGI(TAG, "event received: type=%d", static_cast<int>(evt.type));
    }
  }
}
