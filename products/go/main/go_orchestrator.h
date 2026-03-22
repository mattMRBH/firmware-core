/**
 * AirGradient Go — Orchestrator
 *
 * Central event loop that consumes typed events from producers, updates
 * state machines (app state, UI state, power policy), and calls consumers
 * directly.  See ARCHITECTURE.md section 3 for the high-level design.
 *
 * This is a minimal stub.  The full orchestrator implementation will be
 * added in a subsequent change.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "config_store.h"
#include "go_display.h"
#include "go_events.h"
#include "go_gps.h"
#include "go_input.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_types.h"
#include "go_ui.h"
#include "rtos.h"

class Orchestrator {
public:
  /// References to all product services.  All referenced objects must outlive
  /// the Orchestrator instance.
  struct Services {
    SensorProducer &sensor_producer;
    GpsService &gps_service;
    InputService &input_service;
    DisplayService &display_service;
    StorageService &storage_service;
    PowerService &power_service;
    UIManager &ui_manager;
  };

  /// Construct the orchestrator.
  ///
  /// @param event_queue  RTOS queue handle for the central event queue.
  /// @param services     References to all product services.
  /// @param settings     Product settings (mutable — orchestrator may update
  ///                     on SettingsChanged events).
  /// @param config_store Config store for persisting setting changes.
  Orchestrator(RtosQueueHandle event_queue, Services &services, GoSettings &settings,
               ConfigStore &config_store);

  /// Restore application state and prepare the orchestrator for the main
  /// event loop.  Called once after construction.
  ///
  /// @param cause  Wake cause from PowerService::get_wake_cause().
  void init(WakeCause cause);

  /// Run the main event loop.  Never returns.
  void run();

private:
  RtosQueueHandle _event_queue;
  Services &_services;
  GoSettings &_settings;
  ConfigStore &_config_store;
};
