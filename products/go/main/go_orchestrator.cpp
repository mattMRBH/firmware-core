/**
 * AirGradient Go — Orchestrator implementation
 *
 * Central event loop, event dispatch, timer management, state transitions,
 * display updates, and sleep cycle management.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_orchestrator.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <ctime>
#include <utility>

#include "ag_log.h"
#include "common.h"
#include "go_ble_protocol.h"
#include "go_board.h"
#include "rtos.h"

static constexpr const char *TAG = "Orchestrator";

static constexpr uint8_t SESSION_ID_LENGTH = 5;

static void log_sensor_snapshot(const MeasuresAGo &d) {
  AG_LOGI(TAG,
          "MeasuresAGo:\n"
          "temperature: %.2f\n"
          "humidity: %.2f\n"
          "pm_01: %.1f\n"
          "pm_25: %.1f\n"
          "pm_10: %.1f\n"
          "pm_05_pc: %.0f\n"
          "pm_01_pc: %.0f\n"
          "pm_25_pc: %.0f\n"
          "pm_10_pc: %.0f\n"
          "co2: %d\n"
          "tvoc_index: %d\n"
          "tvoc_raw: %d\n"
          "nox_index: %d\n"
          "nox_raw: %d\n"
          "battery_voltage: %.2f\n"
          "charging_voltage: %.2f\n"
          "pressure: %.1f\n"
          "altitude: %.1f",
          static_cast<double>(d.temp_hum_a.temperature), static_cast<double>(d.temp_hum_a.humidity),
          static_cast<double>(d.pm_a.pm_01), static_cast<double>(d.pm_a.pm_25),
          static_cast<double>(d.pm_a.pm_10), static_cast<double>(d.pm_a.pm_05_pc),
          static_cast<double>(d.pm_a.pm_01_pc), static_cast<double>(d.pm_a.pm_25_pc),
          static_cast<double>(d.pm_a.pm_10_pc), d.co2.co2, d.tvoc_nox.tvoc_index,
          d.tvoc_nox.tvoc_raw, d.tvoc_nox.nox_index, d.tvoc_nox.nox_raw,
          static_cast<double>(d.power.battery_voltage),
          static_cast<double>(d.power.charging_voltage), static_cast<double>(d.pressure.pressure),
          static_cast<double>(d.pressure.altitude));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Orchestrator::Orchestrator(RtosQueueHandle event_queue, const Services &services,
                           GoSettings settings, ConfigStore &config_store, const char *serial)
    : _event_queue(event_queue), _svc(services), _settings(std::move(settings)),
      _config_store(config_store), _serial(serial), _cached_measures() {}

// ---------------------------------------------------------------------------
// Boot initialization
// ---------------------------------------------------------------------------

void Orchestrator::init(WakeCause cause, const BootHandoff &handoff) {
  AG_LOGI(TAG,
          "init: wake_cause=%d display_painted=%d measurement_done=%d "
          "lock=%d",
          static_cast<int>(cause), static_cast<int>(handoff.display_painted),
          static_cast<int>(handoff.measurement_completed),
          static_cast<int>(handoff.initial_lock_state));

  // Mode always comes from persisted settings (NVS) — single source of truth
  _mode = _settings.operating_mode;

  // --- Restore RTC state for wake-from-sleep cases ---
  if (cause != WakeCause::PowerOn) {
    RtcAppState state = _svc.power_service.load_state();
    _behavior = state.behavior;
    _gps_enabled = state.gps_enabled;
    _tracking_active = state.tracking_active;
    _tracking_session_id = state.tracking_session_id;
  }

  // --- Apply initial lock state ---
  if (handoff.initial_lock_state == LockState::Unlocked) {
    if (handoff.display_painted) {
      // Display already shows unlocked UI — set state directly.
      // Do NOT call unlock() which would trigger update_display().
      _lock_state = LockState::Unlocked;
      _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.show_snackbar("Unlocked");
      uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.clear_expired_snackbar(now_ms);
      _snackbar_refresh_deadline_ms = now_ms + SNACKBAR_DURATION_MS + 200;
    } else {
      // Display not yet showing unlocked UI — unlock triggers
      // update_display() which will paint the unlocked frame.
      unlock();
    }
  }

  // --- Seed cached measures from boot ---
  // Fresh measurements take priority over stale RTC snapshot.
  if (handoff.fast_path_measures != nullptr) {
    _cached_measures = *handoff.fast_path_measures;
  } else if (handoff.display_snapshot != nullptr) {
    _cached_measures.co2.co2 = handoff.display_snapshot->co2_ppm;
    _cached_measures.pm_a.pm_25 = handoff.display_snapshot->pm25_ugm3;
    _cached_measures.temp_hum_a.temperature = handoff.display_snapshot->temperature_c;
    _cached_measures.temp_hum_a.humidity = handoff.display_snapshot->humidity_pct;
    _cached_measures.tvoc_nox.tvoc_index = handoff.display_snapshot->tvoc_index;
    _cached_measures.tvoc_nox.nox_index = handoff.display_snapshot->nox_index;
    _cached_measures.pressure.pressure = handoff.display_snapshot->pressure_hpa;
    _cached_measures.pressure.altitude = handoff.display_snapshot->altitude_m;
  }

  // --- Mark first measurement done if boot already measured ---
  if (handoff.measurement_completed) {
    _first_measurement_done = true;
  }

  // Cold-boot splash gate: run_interactive seeds UIManager via show_info()
  // before this point, so detect the splash from the current screen rather
  // than handoff.display_painted (already flipped to true).
  if (!handoff.measurement_completed && handoff.fast_path_measures == nullptr &&
      _svc.ui_manager.current_screen() == Screen::Info) {
    _boot_splash_active = true;
  }

  // --- Resume route if tracking was active before sleep ---
  if (_tracking_active) {
    _svc.storage_service.start_route(_tracking_session_id);
  }

  // --- Common tail ---
  _svc.ui_manager.sync_settings(_settings);

  if (!handoff.measurement_completed) {
    _svc.sensor_producer.request_measurement(1, SensorGroup::All);
  }

  _latest_power = _svc.power_service.poll_bms();

  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_measurement_ms = now;
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now;
  _last_ext_wdt_ms = now;
  if (handoff.initial_lock_state != LockState::Unlocked || !handoff.display_painted) {
    _last_input_ms = now;
  }

  init_ble_if_portable();
  if (_settings.operating_mode == OperatingMode::Stationary) {
    enter_stationary();
  }
}

// ---------------------------------------------------------------------------
// Main event loop
// ---------------------------------------------------------------------------

void Orchestrator::run() {
  AG_LOGI(TAG, "run: entering main event loop");

  while (true) {
    // Sleep check: enter sleep when locked and first measurement is done
    if (_lock_state == LockState::Locked && _first_measurement_done) {
      try_enter_sleep(); // Returns only when sleep conditions are not met
    }

    uint32_t timeout = compute_queue_timeout_ms();
    Event evt{};
    if (RTOS::queue_receive(_event_queue, &evt, timeout)) {
      dispatch(evt);
    }

    check_timers();
  }
}

// ---------------------------------------------------------------------------
// Timer management
// ---------------------------------------------------------------------------

uint32_t Orchestrator::compute_queue_timeout_ms() const {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  uint32_t next = UINT32_MAX;

  // Sensor measurement + BMS deadlines are paused while sensitive services
  // are paused (Provisioning / ProvisioningConfirm).  Their _last_*_ms
  // values stay frozen and are rebased to "now" on resume so the next
  // deadline lands a full interval into the future, not back-to-back to
  // catch up on missed cycles.
  uint32_t interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (!_provisioning_sensitive_services_paused) {
    // Sensor timer deadline
    {
      uint32_t deadline = _last_measurement_ms + interval_ms;
      uint32_t remaining = deadline - now;
      next = std::min(next, remaining);
    }

    // BMS full-telemetry deadline
    uint32_t bms_remaining = (_last_bms_poll_ms + BMS_POLL_INTERVAL_MS) - now;
    next = std::min(next, bms_remaining);

    // BMS fast charging-status deadline
    uint32_t bms_status_remaining = (_last_bms_status_poll_ms + BMS_STATUS_POLL_INTERVAL_MS) - now;
    next = std::min(next, bms_status_remaining);
  }

  // Inactivity deadline (only when unlocked, auto-lock enabled, and not
  // in a setup session — users on Info / Provisioning / ProvisioningConfirm
  // must not get auto-locked out mid-setup).
  if (_lock_state == LockState::Unlocked && _settings.auto_lock_seconds > 0 &&
      !_setup_session_active) {
    uint32_t inact_interval = static_cast<uint32_t>(_settings.auto_lock_seconds) * 1000;
    uint32_t inact_remaining = (_last_input_ms + inact_interval) - now;
    next = std::min(next, inact_remaining);
  }

  // PM pre-wake deadline (not Offline, interval above threshold, prepare
  // not yet sent, not in the sensitive-services pause).
  bool pm_sleep_eligible =
      _mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval_ms);
  if (pm_sleep_eligible && !_pm_prepare_sent && !_provisioning_sensitive_services_paused) {
    uint32_t measure_deadline = _last_measurement_ms + interval_ms;
    uint32_t prepare_deadline = measure_deadline - CONFIG_SENSOR_WARMUP_DURATION_MS;
    uint32_t pm_remaining = prepare_deadline - now;
    next = std::min(next, pm_remaining);
  }

  // Snackbar refresh deadline — moot during a setup session because
  // snackbars are suppressed on session screens, but explicitly skipped
  // here too so the orchestrator does not wake just to clear a no-op.
  if (_snackbar_refresh_deadline_ms != 0 && !_provisioning_sensitive_services_paused) {
    uint32_t sb_remaining = _snackbar_refresh_deadline_ms - now;
    next = std::min(next, sb_remaining);
  }

  // External watchdog deadline — always a candidate (ext WDT is never
  // suppressed during a session, per spec).  Without this, the gated
  // session path can leave `next == UINT32_MAX`, which the overdue clamp
  // below misinterprets as 0 and busy-spins the main loop, starving the
  // IDLE task and tripping the task WDT (~5 s).
  uint32_t ext_wdt_remaining = (_last_ext_wdt_ms + EXT_WDT_INTERVAL_MS) - now;
  next = std::min(next, ext_wdt_remaining);

  // Wi-Fi initial-connect / fallback deadline
  uint32_t wifi_deadline = _svc.wifi.next_deadline_ms();
  if (wifi_deadline != 0) {
    next = std::min(next, wifi_deadline - now);
  }

  // If any deadline already passed, the unsigned subtraction yields a large
  // number — clamp to 0 so check_timers() fires immediately.
  if (next > MAX_REASONABLE_TIMEOUT_MS) {
    next = 0;
  }

  return next;
}

void Orchestrator::check_timers() {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());

  uint32_t interval = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;

  // The following timers are all skipped while sensitive services are
  // paused (Provisioning / ProvisioningConfirm).  Skipping the firing
  // branch — and not the read of _last_*_ms — means the deadline
  // effectively freezes; rebase_periodic_clocks() rolls _last_*_ms
  // forward on session leave so we don't fire back-to-back catch-up.
  if (!_provisioning_sensitive_services_paused) {
    // --- PM pre-wake timer (fires warmup_duration before next measurement) ---
    bool pm_sleep_eligible =
        _mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval);
    if (pm_sleep_eligible && !_pm_prepare_sent) {
      uint32_t measure_deadline = _last_measurement_ms + interval;
      uint32_t prepare_deadline = measure_deadline - CONFIG_SENSOR_WARMUP_DURATION_MS;
      if ((now - prepare_deadline) < MAX_REASONABLE_TIMEOUT_MS) {
        AG_LOGI(TAG, "PM pre-wake: powering on and requesting prepare");
        _svc.power_service.set_pm_power(true);
        _svc.sensor_producer.request_prepare();
        _pm_prepare_sent = true;
      }
    }

    // --- Sensor timer (single) ---
    if ((now - _last_measurement_ms) >= interval) {
      _svc.sensor_producer.request_measurement(1, SensorGroup::All);
      _last_measurement_ms = now;
      _pm_prepare_sent = false;
    }

    // --- BMS full telemetry timer ---
    if ((now - _last_bms_poll_ms) >= BMS_POLL_INTERVAL_MS) {
      on_bms_timer();
    }

    // --- BMS fast charging-status timer (between full polls) ---
    if ((now - _last_bms_status_poll_ms) >= BMS_STATUS_POLL_INTERVAL_MS) {
      on_bms_status_timer();
    }

    // --- Snackbar refresh timer ---
    // Snackbars are suppressed on session screens, so this is a no-op
    // there in any case; the explicit gate avoids spurious wake-ups.
    if (_snackbar_refresh_deadline_ms != 0 &&
        (now - _snackbar_refresh_deadline_ms) < MAX_REASONABLE_TIMEOUT_MS) {
      _snackbar_refresh_deadline_ms = 0;
      request_background_display_update();
    }
  }

  // --- External watchdog timer (never suppressed — must keep running
  // during the session or the device reboots mid-setup) ---
  if ((now - _last_ext_wdt_ms) >= EXT_WDT_INTERVAL_MS) {
    _svc.power_service.reset_ext_watchdog();
    _last_ext_wdt_ms = now;
  }

  // --- Auto-lock timer (suppressed across the entire setup session,
  // including Screen::Info where sensitive services keep running, and on
  // focus screens like PairingPasskey where a lock-and-redraw would
  // interrupt the user mid-action) ---
  if (_lock_state == LockState::Unlocked && _settings.auto_lock_seconds > 0 &&
      !_setup_session_active && !_svc.ui_manager.is_focus_screen()) {
    uint32_t inact_interval = static_cast<uint32_t>(_settings.auto_lock_seconds) * 1000;
    if ((now - _last_input_ms) >= inact_interval) {
      on_inactivity_timeout();
    }
  }

  // --- Wi-Fi service tick (clears expired deadline / fires synthetic disco) ---
  _svc.wifi.tick(now);
}

void Orchestrator::on_bms_timer() {
  log_heap(TAG, "bms.timer:tick");
  _latest_power = _svc.power_service.poll_bms();
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now; // Full poll subsumes the fast status check.

  if (_latest_power.ship_mode_request != ShipModeRequest::None) {
    shutdown(_latest_power.ship_mode_request);
    return; // system is shutting down — skip further processing
  }

  // Update BLE status characteristic with latest power/GPS/tracking state
  if (_svc.ble_service.is_initialized()) {
    _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                   _tracking_session_id);
  }
}

void Orchestrator::on_bms_status_timer() {
  BmsStatus status{};
  if (_svc.power_service.poll_status(status)) {
    bool was_charging = is_bms_charging(_latest_power.charging_status);
    const BmsPowerSource previous_power_source = _latest_power.charger_status.power_source;
    _latest_power.charging_status = status.charging_state;
    _latest_power.charger_status = status;
    bool now_charging = is_bms_charging(status.charging_state);
    if (was_charging != now_charging || previous_power_source != status.power_source) {
      AG_LOGI(
          TAG, "charger status changed: charge=%s -> %s source=%s -> %s",
          was_charging ? "charging" : "not charging", now_charging ? "charging" : "not charging",
          bms_power_source_str(previous_power_source), bms_power_source_str(status.power_source));
      request_background_display_update();
    }
  }

  _last_bms_status_poll_ms = static_cast<uint32_t>(RTOS::get_time_ms());
}

void Orchestrator::on_inactivity_timeout() { lock(); }

void Orchestrator::reschedule_sensor_timer(const GoSettings &previous_settings) {
  if (previous_settings.measure_interval_seconds == _settings.measure_interval_seconds) {
    return;
  }
  _last_measurement_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Reconcile PM power with the new interval.  Idempotent GPIO writes.
  uint32_t new_interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline &&
      _svc.power_service.should_sleep_pm_sensor(new_interval_ms)) {
    _svc.power_service.set_pm_power(false);
  } else {
    _svc.power_service.set_pm_power(true);
  }
}

// ---------------------------------------------------------------------------
// Event dispatch
// ---------------------------------------------------------------------------

void Orchestrator::dispatch(const Event &event) {
  switch (event.type) {
  case EventType::SensorDataReady:
    on_sensor_data(event.sensor_data);
    break;
  case EventType::GpsFixUpdate:
    on_gps_fix(event.gps_data);
    break;
  case EventType::InputPress:
    on_input(event.input);
    break;

  // BLE events
  case EventType::BleConnected:
    on_ble_connected();
    break;
  case EventType::BleDisconnected:
    on_ble_disconnected();
    break;
  case EventType::BleConfigWrite:
    on_ble_config_write();
    break;
  case EventType::BleHistoryWrite:
    on_ble_history_write();
    break;
  case EventType::BlePairingRequest:
    on_ble_pairing_request(event.ble_passkey);
    break;
  case EventType::BleAuthComplete:
    on_ble_auth_complete();
    break;

  // Calibration events
  case EventType::Co2CalibrationDone:
    on_co2_calibration_done(static_cast<Co2CalibrationResult>(event.co2_cal_result));
    break;

  // UI action events (reserved for future programmatic triggers)
  case EventType::UserStartTracking:
    start_tracking();
    break;
  case EventType::UserStopTracking:
    stop_tracking();
    break;
  case EventType::UserChangeMode:
    change_mode(event.mode_change);
    break;
  case EventType::UserToggleGps:
    _gps_enabled = event.gps_enabled;
    break;
  case EventType::SettingsChanged:
    apply_settings_change();
    break;
  case EventType::ClearData:
    clear_data();
    break;
  case EventType::SaveTag:
    save_tag(event.tag_index, nullptr);
    break;

  // System events
  case EventType::InactivityTimeout:
    on_inactivity_timeout();
    break;
  case EventType::MeasurementTimer:
    check_timers(); // legacy event: just re-check all timers
    break;
  case EventType::WakeFromSleep:
    break; // wake handled in init()

  // Wi-Fi
  case EventType::WifiConnected:
    on_wifi_connected(event.wifi_ip);
    break;
  case EventType::WifiDisconnected:
    on_wifi_disconnected(static_cast<WifiDisconnectReason>(event.wifi_disconnect_reason));
    break;

  case EventType::ProvisioningStateChanged:
    on_provisioning_state_changed(event.prov);
    break;

  case EventType::PostMeasuresResult:
    AG_LOGI(TAG, "post_measures result=%d", static_cast<int>(event.cloud_result));
    break;

  case EventType::FetchConfigResult:
    AG_LOGI(TAG, "fetch_config result=%d", static_cast<int>(event.cloud_result));
    break;
  }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void Orchestrator::on_sensor_data(const MeasuresAGo &data) {
  // Always overwrite all fields — single interval, no group-based gating.
  _cached_measures.pm_a = data.pm_a;
  _cached_measures.co2 = data.co2;
  _cached_measures.temp_hum_a = data.temp_hum_a;
  _cached_measures.tvoc_nox = data.tvoc_nox;
  _cached_measures.pressure = data.pressure;
  _cached_measures.power = data.power;

  _first_measurement_done = true;

  // Hand off the "Booting..." splash to Home. Skip if a setup session
  // owns Info (Stationary bring-up drives its own Info -> Home).
  if (_boot_splash_active) {
    _boot_splash_active = false;
    if (!_setup_session_active && _svc.ui_manager.current_screen() == Screen::Info) {
      _svc.ui_manager.reset_to_home();
    }
  }

  log_sensor_snapshot(_cached_measures);

  _svc.storage_service.cache_measurement(_cached_measures);

  // Unconditional — snapshot is ready for the next Stationary arm.
  _svc.cloud.update_measures_snapshot(_cached_measures);

  if (_tracking_active) {
    RoutePoint point{};
    point.timestamp = time(nullptr);
    point.gps = _latest_gps;
    point.sensors = _cached_measures;
    point.battery_percentage = _latest_power.battery_percentage;
    _svc.storage_service.append_route_point(point);
  }

  // Update BLE measures characteristic (always for READ; notifies when connected)
  _svc.ble_service.notify_measures(_cached_measures, _latest_gps, time(nullptr));

  // Power off PM sensor after measurement when interval justifies power-cycling.
  uint32_t interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval_ms)) {
    _svc.power_service.set_pm_power(false);
  }

  request_background_display_update();
}

void Orchestrator::on_co2_calibration_done(Co2CalibrationResult result) {
  switch (result) {
  case Co2CalibrationResult::Success:
    AG_LOGI(TAG, "CO2 calibration succeeded");
    _svc.ui_manager.show_snackbar("CO2 cal. done");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, true);
    break;
  case Co2CalibrationResult::Unsupported:
    AG_LOGW(TAG, "CO2 calibration unsupported by sensor");
    _svc.ui_manager.show_snackbar("CO2 cal. unsupported");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, false,
                                           BLE_VAL_ERR_UNSUPPORTED);
    break;
  case Co2CalibrationResult::Failed:
    AG_LOGW(TAG, "CO2 calibration failed");
    _svc.ui_manager.show_snackbar("CO2 cal. failed");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, false,
                                           BLE_VAL_ERR_CALIBRATION_FAILED);
    break;
  }
  update_display();
}

void Orchestrator::on_gps_fix(const GpsData &data) {
  if (!is_gps_active()) {
    return; // GPS disabled in settings; ignore
  }
  _latest_gps = data;

  AG_LOGI(TAG, "gps_fix: lat=%.6f lon=%.6f alt=%.1f fix=%d sat=%d hdop=%.1f",
          data.position.latitude, data.position.longitude, data.altitude_m,
          static_cast<int>(data.fix.fix_type), data.fix.satellite_count, data.fix.hdop);
}

static const char *input_source_str(InputSource s) {
  switch (s) {
  case InputSource::TouchUp:
    return "TouchUp";
  case InputSource::TouchDown:
    return "TouchDown";
  case InputSource::TouchEnter:
    return "TouchEnter";
  case InputSource::ButtonPower:
    return "BtnPower";
  case InputSource::ButtonBoot:
    return "BtnBoot";
  default:
    return "Unknown";
  }
}

void Orchestrator::on_input(const InputEventData &input) {
  AG_LOGI(TAG, "input: source=%s type=%s lock=%s", input_source_str(input.source),
          input.type == InputType::ShortPress ? "short" : "long",
          _lock_state == LockState::Locked ? "locked" : "unlocked");

  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Shutdown: long press on power button (any lock state)
  if (input.source == InputSource::ButtonPower && input.type == InputType::LongPress) {
    shutdown();
    return;
  }

  // Factory reset: long press on boot button
  if (input.source == InputSource::ButtonBoot && input.type == InputType::LongPress) {
    if (factory_reset()) {
      AG_LOGI(TAG, "Rebooting in 2s");
      RTOS::delay_ms(2000);
      reboot();
    }
    return;
  }

  // Power short-press: lock toggle is suppressed on session screens so a
  // mid-setup press cannot lock the device and hide the on-phone
  // instructions the user is following.  Power long-press shutdown (above)
  // and Boot long-press factory-reset (above) remain functional as
  // recovery paths.
  if (input.source == InputSource::ButtonPower && input.type == InputType::ShortPress) {
    if (_setup_session_active || _boot_splash_active) {
      return; // suppressed on setup-session screens and during cold-boot splash
    }
    if (_lock_state == LockState::Locked) {
      unlock();
    } else {
      lock();
    }
    return;
  }

  // Locked: show hint on touch input, ignore otherwise
  if (_lock_state == LockState::Locked) {
    if (input.source == InputSource::TouchUp || input.source == InputSource::TouchDown ||
        input.source == InputSource::TouchEnter) {
      _svc.ui_manager.show_snackbar("Unlock First");
      update_display();
    }
    return;
  }

  // Unlocked: forward to UI Manager
  UIActionResult result = _svc.ui_manager.handle_input(input.source, input.type);

  switch (result.action) {
  case UIAction::StartTracking:
    start_tracking();
    break;
  case UIAction::StopTracking:
    stop_tracking();
    break;
  case UIAction::ChangeMode:
    change_mode(result.new_mode);
    break;
  case UIAction::SettingsChanged:
    apply_settings_change();
    break;
  case UIAction::ClearData:
    clear_data();
    break;
  case UIAction::CalibrateCo2:
    _svc.sensor_producer.request_co2_calibration();
    _svc.ui_manager.show_snackbar("Calibrating CO2...");
    break;
  case UIAction::SaveTag:
    save_tag(result.tag_index, result.tag_label);
    break;
  case UIAction::ConfirmCancelProvisioning:
    // Cancel-setup confirmed — drop back to Portable via the session
    // leave path so battery / clocks / snackbar state are all restored
    // cleanly.  The helper does its own update_display + flush, so we
    // return before the catch-all tail render below.
    leave_session_to_portable();
    return;
  case UIAction::ConfirmSwitchProvisioningTransport:
    // Latch the transient "Switching to ..." state on the page first so
    // the user sees an ack before switch_provisioning_transport()'s
    // back-to-back stop()+start() fires further status events that
    // would overwrite the transient frame.  wait=true + flush() guarantee
    // the ack is painted before the transport flips.  Returns early to
    // bypass the catch-all tail render.
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::SwitchingTransport);
    update_display(/*wait=*/true);
    _svc.display_service.flush();
    _svc.wifi.switch_provisioning_transport();
    return;
  case UIAction::None:
    break;
  }

  // Touch-driven session transitions (row toggle, confirm open, No/Yes
  // toggle, No-back) must queue drop-free so the new frame is not lost
  // when the worker is mid-paint on a prior frame.  Non-session screens
  // keep the existing non-blocking semantics.
  update_display(_setup_session_active);
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

void Orchestrator::lock() {
  AG_LOGI(TAG, "lock");
  _svc.ui_manager.show_snackbar("Locked");
  _lock_state = LockState::Locked;
  _svc.ui_manager.reset_to_home();
  update_display();
}

void Orchestrator::unlock() {
  AG_LOGI(TAG, "unlock");
  _svc.ui_manager.show_snackbar("Unlocked");
  _lock_state = LockState::Unlocked;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  update_display();
}

void Orchestrator::start_tracking() {
  if (_tracking_active) {
    return;
  }

  AG_LOGI(TAG, "start_tracking");
  const bool was_gps_active = is_gps_active();
  _tracking_session_id = generate_session_id();
  _tracking_active = true;
  _behavior = Behavior::Tracking;

  if (!was_gps_active && is_gps_active()) {
    _svc.gps_service.start();
  }

  _svc.storage_service.start_route(_tracking_session_id);
  char msg[48];
  (void)snprintf(msg, sizeof(msg), "Tracking start = %05" PRIu32, _tracking_session_id);
  _svc.ui_manager.show_snackbar(msg);
  update_display();
}

void Orchestrator::stop_tracking() {
  if (!_tracking_active) {
    return;
  }

  AG_LOGI(TAG, "stop_tracking");
  const bool was_gps_active = is_gps_active();
  const uint32_t ended_session_id = _tracking_session_id;
  _svc.storage_service.end_route();
  _tracking_active = false;
  _tracking_session_id = 0;
  _behavior = Behavior::Idle;

  if (was_gps_active && !is_gps_active()) {
    deactivate_gps();
  }

  char msg[48];
  (void)snprintf(msg, sizeof(msg), "Tracking stop = %05" PRIu32, ended_session_id);
  _svc.ui_manager.show_snackbar(msg);
  update_display();
}

void Orchestrator::change_mode(OperatingMode new_mode) {
  OperatingMode old_mode = _mode;
  AG_LOGI(TAG, "change_mode: %d -> %d", static_cast<int>(old_mode), static_cast<int>(new_mode));
  log_heap(TAG, "mode.change:enter");
  _mode = new_mode;
  _settings.operating_mode = new_mode;
  save_go_settings(_config_store, _settings);
  // Keep UIManager's cached _setting_mode index in lockstep with the
  // persisted GoSettings::operating_mode.  Without this, paths that change
  // the mode without going through apply_setting_choice (e.g. the
  // cancel-from-provisioning leave routing through change_mode(Portable))
  // leave the Settings menu showing the previously-selected option.
  _svc.ui_manager.sync_settings(_settings);

  // Two-phase: tear down outgoing mode, then bring up incoming.
  if (old_mode == OperatingMode::Portable && new_mode != OperatingMode::Portable) {
    _svc.ui_manager.dismiss_pairing_passkey();
    _svc.ble_service.deinit();
  }
  if (old_mode == OperatingMode::Stationary && new_mode != OperatingMode::Stationary) {
    // Cloud before Wi-Fi: drain in-flight HTTP while socket is alive.
    _svc.cloud.disarm();
    _svc.cloud.stop();
    _svc.wifi.shutdown();
    resume_provisioning_sensitive_services();
    _cloud_first_post_pending = false;
  }
  log_heap(TAG, "mode.change:after-teardown");

  if (new_mode == OperatingMode::Portable && old_mode != OperatingMode::Portable) {
    init_ble_if_portable();
  }

  // Ensure PM sensor is powered on — covers mode changes away from
  // Portable while PM was power-cycled off.  Idempotent.  Must fire
  // before the Stationary early-return below so Stationary entry still
  // re-enables the PM rail after a prior Portable session may have
  // power-cycled it off.
  _svc.power_service.set_pm_power(true);

  if (new_mode == OperatingMode::Stationary && old_mode != OperatingMode::Stationary) {
    // enter_stationary() opens Screen::Info with the bring-up text and
    // calls update_display(wait=true) itself.  Skip the generic
    // "Mode changed" snackbar + update_display() so the Info text is
    // not stomped.
    enter_stationary();
    return;
  }

  _svc.ui_manager.show_snackbar("Mode changed");
  update_display();
  log_heap(TAG, "mode.change:after-bringup");
}

void Orchestrator::apply_settings_change() {
  const bool was_gps_active = is_gps_active();
  const GoSettings previous_settings = _settings;
  _svc.ui_manager.apply_to_settings(_settings);
  save_go_settings(_config_store, _settings);

  // Propagate runtime changes to services
  reschedule_sensor_timer(previous_settings);
  _svc.gps_service.set_posting_interval_ms(_settings.gps_interval_seconds * 1000);

  if (_settings.disable_cloud != previous_settings.disable_cloud) {
    _svc.cloud.set_disable_cloud(_settings.disable_cloud);
  }

  const bool is_gps_active_now = is_gps_active();
  if (!was_gps_active && is_gps_active_now) {
    _svc.gps_service.start();
  } else if (was_gps_active && !is_gps_active_now) {
    deactivate_gps();
  }

  _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);

  // Notify connected BLE client of config change
  if (_svc.ble_service.is_connected()) {
    _svc.ble_service.notify_config(_settings);
    _svc.ble_service.update_config(_settings);
  }
}

bool Orchestrator::clear_data() {
  if (_tracking_active) {
    stop_tracking();
  }

  _svc.storage_service.clear_cache();
  const bool routes_cleared = _svc.storage_service.clear_routes();

  if (_svc.ble_service.is_connected()) {
    _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                   _tracking_session_id);
  }

  _svc.ui_manager.show_snackbar(routes_cleared ? "Data cleared" : "Data clear failed");
  update_display();

  return routes_cleared;
}

bool Orchestrator::factory_reset() {
  AG_LOGI(TAG, "factory_reset");

  // Erase temporary cache data and delete all persisted route files.
  const bool data_cleared = clear_data();

  const GoSettings defaults{};

  // Overwrite persisted product settings with their default values.
  // Zeros disable_cloud + static_ip as a side effect.
  const bool settings_saved = save_go_settings(_config_store, defaults);

  // Erase ESP-IDF Wi-Fi NVS credentials and reset online latches.
  _svc.wifi.clear_credentials();

  // Delete all stored BLE bond information.
  const bool bonds_cleared = _svc.ble_service.delete_all_bonds();

  const bool success = data_cleared && settings_saved && bonds_cleared;

  if (!success) {
    _svc.ui_manager.show_snackbar("Factory reset failed");
    update_display();
    return false;
  }

  AG_LOGI(TAG, "Factory reset success");

  _settings = defaults;
  _mode = _settings.operating_mode;
  _behavior = Behavior::Idle;
  _lock_state = LockState::Locked;
  _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);
  _tracking_active = false;
  _tracking_session_id = 0;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  _svc.ui_manager.sync_settings(_settings);
  _svc.ui_manager.reset_to_home();
  update_display();

  return true;
}

void Orchestrator::save_tag(uint8_t tag_index, const char *tag_label) {
  (void)tag_index;
  (void)tag_label;
  // TODO: persist tag association with current route point via StorageService
  _svc.ui_manager.show_snackbar("Tag saved");
  update_display();
}

void Orchestrator::shutdown(ShipModeRequest reason) {
  AG_LOGI(TAG, "shutdown (reason=%d)", static_cast<int>(reason));

  // 1. Map shutdown reason to the matching screen variant.
  Screen screen;
  switch (reason) {
  case ShipModeRequest::OverDischarge:
    screen = Screen::ShutdownDischarge;
    break;
  case ShipModeRequest::OverTemperature:
    screen = Screen::ShutdownTemperature;
    break;
  case ShipModeRequest::None:
  default:
    screen = Screen::ShutdownUser;
    break;
  }
  _svc.ui_manager.set_screen(screen);
  update_display(); // starts e-paper refresh (async)

  // 2. Persist state (runs while display refreshes).
  if (_tracking_active) {
    stop_tracking();
  }
  _svc.storage_service.backup_cache();

  // 3. Disable peripherals to reduce power draw before final shutdown.
  _svc.power_service.set_pm_power(false);
  // TODO: stop GPS when the method is available

  // 4. Wait for e-paper refresh to complete.
  RTOS::delay_ms(SHUTDOWN_DISPLAY_DELAY_MS);

  // 5. Ship mode → deep sleep fallback.
  _svc.power_service.shutdown(); // BMS QoN — does not return
}

// ---------------------------------------------------------------------------
// BLE event handlers
// ---------------------------------------------------------------------------

void Orchestrator::on_ble_connected() {
  AG_LOGI(TAG, "BLE client connected");

  // Dismiss pairing passkey screen if it was showing
  _svc.ui_manager.dismiss_pairing_passkey();

  // Push current state to BLE characteristics
  _svc.ble_service.notify_measures(_cached_measures, _latest_gps, time(nullptr));
  _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                 _tracking_session_id);
  _svc.ble_service.update_config(_settings);

  request_background_display_update();
}

void Orchestrator::on_ble_disconnected() {
  AG_LOGI(TAG, "BLE client disconnected");
  _svc.ui_manager.dismiss_pairing_passkey();
  request_background_display_update();
}

void Orchestrator::on_ble_auth_complete() {
  AG_LOGI(TAG, "BLE auth complete");
  _svc.ui_manager.dismiss_pairing_passkey();
  request_background_display_update();
}

void Orchestrator::on_ble_config_write() {
  uint8_t buf[BLE_WRITE_BUF_SIZE];
  size_t len = _svc.ble_service.take_pending_config_write(buf, sizeof(buf));
  if (len == 0) {
    return;
  }

  // Decode into a copy of current settings (merge approach)
  GoSettings temp = _settings;
  BleConfigDecodeResult result = BleService::decode_config_write(buf, len, temp);

  // Reject writes that contain unrecognized config keys
  if (result.op == BleConfigOp::Set && result.has_unknown_keys) {
    AG_LOGW(TAG, "BLE config set rejected: unknown config key");
    _svc.ble_service.notify_command_result(BleCommand::Set, false, BLE_VAL_ERR_UNKNOWN_CONFIG_KEY);
    return;
  }

  switch (result.op) {
  case BleConfigOp::Set: {
    AG_LOGI(TAG, "BLE config set");
    const bool was_gps_active = is_gps_active();
    const GoSettings previous_settings = _settings;
    _settings = temp;
    save_go_settings(_config_store, _settings);

    // Propagate runtime changes
    reschedule_sensor_timer(previous_settings);
    _svc.gps_service.set_posting_interval_ms(_settings.gps_interval_seconds * 1000);
    _svc.ui_manager.sync_settings(_settings);

    // Notify BLE client with updated full config
    _svc.ble_service.notify_config(_settings);
    _svc.ble_service.update_config(_settings);

    const bool is_gps_active_now = is_gps_active();
    if (!was_gps_active && is_gps_active_now) {
      _svc.gps_service.start();
    } else if (was_gps_active && !is_gps_active_now) {
      deactivate_gps();
    }
    _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);

    // Check if operating mode changed via BLE
    if (_settings.operating_mode != _mode) {
      change_mode(_settings.operating_mode);
    }

    request_background_display_update();
    break;
  }
  case BleConfigOp::Command: {
    AG_LOGI(TAG, "BLE command: %d", static_cast<int>(result.cmd));

    switch (result.cmd) {
    case BleCommand::Co2Calibration:
      // Runs asynchronously on the SensorProducer task.
      // Result arrives via Co2CalibrationDone event.
      _svc.ble_service.notify_command_progress(result.cmd);
      _svc.sensor_producer.request_co2_calibration();
      break;
    case BleCommand::ClearData: {
      _svc.ble_service.notify_command_progress(result.cmd);
      const bool cleared = clear_data();
      _svc.ble_service.notify_command_result(result.cmd, cleared,
                                             cleared ? nullptr : BLE_VAL_ERR_CLEAR_FAILED);
    } break;
    case BleCommand::FactoryReset: {
      _svc.ble_service.notify_command_progress(result.cmd);
      const bool reset = factory_reset();
      _svc.ble_service.notify_command_result(result.cmd, reset,
                                             reset ? nullptr : BLE_VAL_ERR_FACTORY_RESET_FAILED);
      if (reset) {
        AG_LOGI(TAG, "Rebooting in 2s");
        RTOS::delay_ms(2000);
        reboot();
      }
    } break;
    case BleCommand::StartTracking: {
      const bool was_idle = !_tracking_active;
      start_tracking();
      _svc.ble_service.notify_command_result(result.cmd, was_idle,
                                             was_idle ? nullptr : BLE_VAL_ERR_ALREADY_TRACKING);
    } break;
    case BleCommand::StopTracking: {
      const bool was_tracking = _tracking_active;
      stop_tracking();
      _svc.ble_service.notify_command_result(result.cmd, was_tracking,
                                             was_tracking ? nullptr : BLE_VAL_ERR_NOT_TRACKING);
    } break;
    case BleCommand::SetAiding: {
      const bool has_time = has_aiding_time(result.aiding);
      const bool has_data = has_aiding_position(result.aiding) || has_time;
      if (has_data) {
        _svc.gps_service.set_aiding_data(result.aiding);
      }
      if (has_time) {
        // GPSService might delay the execution, hence if time available, still can sync it
        RTOS::set_system_time_from_epoch(result.aiding.epoch_s);
      }
      _svc.ble_service.notify_command_result(result.cmd, has_data,
                                             has_data ? nullptr : BLE_VAL_ERR_NO_AIDING_DATA);
    } break;
    case BleCommand::Set:
      // Not reachable via Command op — handled above as config-set rejection
      break;
    case BleCommand::Unknown:
      AG_LOGW(TAG, "BLE unknown command");
      _svc.ble_service.notify_command_result(result.cmd, false, BLE_VAL_ERR_UNKNOWN_COMMAND);
      break;
    }
    break;
  }
  case BleConfigOp::Invalid:
    AG_LOGW(TAG, "BLE config write: invalid CBOR");
    break;
  }
}

void Orchestrator::on_ble_history_write() {
  uint8_t buf[BLE_WRITE_BUF_SIZE];
  size_t len = _svc.ble_service.take_pending_history_write(buf, sizeof(buf));
  if (len == 0) {
    return;
  }

  BleHistoryDecodeResult result = BleService::decode_history_write(buf, len);

  switch (result.op) {
  case BleHistoryOp::List:
    AG_LOGI(TAG, "BLE history: list");
    _svc.ble_service.handle_history_list();
    break;
  case BleHistoryOp::Start:
    AG_LOGI(TAG, "BLE history: start session %" PRIu32, result.session_id);
    _svc.ble_service.handle_history_start(result.session_id);
    break;
  case BleHistoryOp::Fill:
    AG_LOGI(TAG, "BLE history: fill %u points", static_cast<unsigned>(result.point_count));
    _svc.ble_service.handle_history_fill(result.point_indices, result.point_count);
    break;
  case BleHistoryOp::End:
    AG_LOGI(TAG, "BLE history: end");
    _svc.ble_service.handle_history_end();
    break;
  case BleHistoryOp::Delete:
    AG_LOGI(TAG, "BLE history: delete session %" PRIu32, result.session_id);
    if (_tracking_active && _tracking_session_id == result.session_id) {
      _svc.ble_service.notify_history_error(BLE_VAL_ERR_SESSION_ACTIVE);
    } else {
      _svc.ble_service.handle_history_delete(result.session_id);
      _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                     _tracking_session_id);
    }
    break;
  case BleHistoryOp::Invalid:
    AG_LOGW(TAG, "BLE history write: invalid CBOR");
    break;
  }
}

void Orchestrator::on_ble_pairing_request(uint32_t passkey) {
  AG_LOGI(TAG, "BLE pairing request: passkey=%06" PRIu32, passkey);
  _svc.ui_manager.show_pairing_passkey(passkey);
  update_display();
}

void Orchestrator::init_ble_if_portable() {
  if (_mode != OperatingMode::Portable) {
    return;
  }

  if (_svc.ble_service.is_initialized()) {
    return; // already running
  }

  log_heap(TAG, "ble.init:pre");
  if (!_svc.ble_service.init(_serial)) {
    AG_LOGE(TAG, "BLE init failed");
  }
  log_heap(TAG, "ble.init:post");
}

// ---------------------------------------------------------------------------
// Stationary Wi-Fi
// ---------------------------------------------------------------------------

void Orchestrator::enter_stationary() {
  log_heap(TAG, "wifi.enter-stationary:enter");
  // Idempotent — cheap no-op on warm Stationary re-entry. Portable-only
  // boots never reach this line.
  _svc.board.init_wifi_subsystem();
  log_heap(TAG, "wifi.enter-stationary:after-init");

  // Silent unlock + snackbar clear.  Required so a cold-boot Locked
  // device can interact with the session screens (Info / Provisioning),
  // and so leftover snackbars cannot leak onto session screens or fire
  // when we eventually return to Home.  Idempotent — see
  // begin_session_if_needed().
  begin_session_if_needed();

  _bring_up_pending = true;

  // Configure cloud state now; defer start() to the first-online callback
  // so the heap-heavy task doesn't exist during provisioning.
  _cloud_first_post_pending = true;
  _svc.cloud.set_disable_cloud(_settings.disable_cloud);

  if (_svc.wifi.has_saved_credentials()) {
    const WifiStaticIpConfig *ip = _settings.static_ip.ip != 0 ? &_settings.static_ip : nullptr;
    AG_LOGI(TAG, "stationary: saved credentials %s static IP", ip != nullptr ? "with" : "without");
    _svc.ui_manager.show_info("Connecting to saved Wi-Fi...");
    _svc.wifi.connect_with_saved_credentials(ip);
  } else {
    AG_LOGI(TAG, "stationary: no credentials — trying default fallback");
    _svc.ui_manager.show_info("Trying default Wi-Fi...");
    _svc.wifi.try_default_fallback_credentials();
  }

  // Full refresh — entering the setup session boundary.  wait=true so
  // the Info frame is queued even if the worker is still painting a
  // prior frame.
  update_display(/*wait=*/true);
}

void Orchestrator::begin_session_if_needed() {
  if (_setup_session_active) {
    return; // Already inside the session (Info -> Provisioning transition).
  }
  _setup_session_active = true;

  // Silent unlock — no snackbar, no display side effect.  Guarantees a
  // cold-boot Locked device can interact with the session screens, and
  // prevents the leave-to-Home transition from firing the "Unlocked"
  // snackbar on success.
  _lock_state = LockState::Unlocked;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Clear any pending snackbar so a leftover "Mode changed", "Locked",
  // "Unlocked", or stale "Wi-Fi connected" cannot leak onto the session
  // screens or fire when the device eventually returns to Home.
  _svc.ui_manager.show_snackbar(nullptr);
  _snackbar_refresh_deadline_ms = 0;
}

void Orchestrator::enter_provisioning_page(ProvisioningTransport transport) {
  AG_LOGI(TAG, "enter_provisioning_page: transport=%u", static_cast<unsigned>(transport));
  // Idempotent — no-op if Info already set up the session; otherwise
  // performs silent unlock + snackbar clear so a post-online auth_failed
  // entry from Home lands on the page in a clean state.
  begin_session_if_needed();

  // Stop the on-Info bring-up arm from acting on any further events
  // (a late WifiConnected post-handoff would otherwise try to render
  // "Connected!" on the wrong screen).
  _bring_up_pending = false;

  pause_provisioning_sensitive_services();
  _svc.ui_manager.open_provisioning(transport);
  log_heap(TAG, "prov.enter-page:pre");
  _svc.wifi.start_provisioning(transport);
  log_heap(TAG, "prov.enter-page:post");
  // Full refresh — session boundary, or Info -> Provisioning jump (the
  // refresh policy in DisplayService::update() picks Full in both cases).
  update_display(/*wait=*/true);
}

void Orchestrator::rebase_periodic_clocks() {
  const uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_measurement_ms = now;
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now;
  // _last_ext_wdt_ms is deliberately not rebased — ext WDT was not paused.
  // _last_input_ms is set by lock()/unlock() and on_input(); not touched here.
}

void Orchestrator::leave_session_to_home() {
  AG_LOGI(TAG, "leave_session_to_home");
  _svc.ui_manager.set_provisioning_connected(0);
  _svc.ui_manager.reset_to_home();
  _bring_up_pending = false;

  // Fresh battery snapshot before resume requests an immediate measurement.
  _latest_power = _svc.power_service.poll_bms();

  // Keep _setup_session_active = true through resume so any background
  // render path (e.g. the immediate measurement that resume requests, a
  // stray BLE-status event) still no-ops through the suppression gate.
  // Cleared just before the final blocking render below.
  resume_provisioning_sensitive_services(); // no-op if not paused (Info exit)
  rebase_periodic_clocks();

  // Silent unlock — page already showed success; no "Unlocked" snackbar.
  _lock_state = LockState::Unlocked;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  _setup_session_active = false; // gate cleared after teardown completes
  update_display(/*wait=*/true);
  _svc.display_service.flush(); // paint completes before caller returns
}

void Orchestrator::leave_session_to_portable() {
  AG_LOGI(TAG, "leave_session_to_portable");
  _svc.ui_manager.set_provisioning_connected(0);
  // reset_to_home() first so change_mode()'s update_display() and the
  // subsequent explicit render below both build values against
  // Screen::Home (change_mode does not change the UI screen on its own).
  _svc.ui_manager.reset_to_home();
  _bring_up_pending = false;
  _latest_power = _svc.power_service.poll_bms();

  // Keep _setup_session_active = true through change_mode() so any
  // background-render path that fires mid-teardown (resume's immediate
  // measurement, a stray BLE event during init_ble_if_portable()) still
  // no-ops through the suppression gate.
  change_mode(OperatingMode::Portable);
  rebase_periodic_clocks();

  _setup_session_active = false; // gate cleared after teardown completes
  update_display(/*wait=*/true); // rescues any drop from change_mode's render
  _svc.display_service.flush();  // paint completes before caller returns
}

void Orchestrator::on_wifi_connected(uint32_t ip) {
  AG_LOGI(TAG, "wifi connected: ip=0x%08x", static_cast<unsigned>(ip));
  if (_mode != OperatingMode::Stationary) {
    return; // ignore stray late events on non-Stationary modes
  }

  if (_bring_up_pending) {
    // Initial Stationary bring-up STA success: show "Connected!\n<ip>"
    // on Screen::Info, hold STA_SUCCESS_HOLD_MS post-paint, then leave
    // the session to Home unlocked.  No snackbar — the on-page text
    // already conveys success.
    _bring_up_pending = false;

    char ip_str[16];
    format_ipv4_be(ip, ip_str);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Connected!\n%s", ip_str);
    _svc.ui_manager.show_info(buf);
    update_display(/*wait=*/true); // queue the success frame, no drop
    _svc.display_service.flush();  // wait until paint completes
    RTOS::delay_ms(STA_SUCCESS_HOLD_MS);

    leave_session_to_home();
  } else if (!_setup_session_active && _svc.ui_manager.current_screen() == Screen::Home) {
    // Post-online reconnect on Home — keep the existing snackbar.
    _svc.ui_manager.show_snackbar("Wi-Fi connected");
    update_display();
  }
  // Cloud arm: unconditional on every Stationary IP transition.
  // start() is idempotent (no-op on reconnect, heap-claim on first call).
  if (!_svc.cloud.start()) {
    AG_LOGE(TAG, "cloud.start() failed; cloud transport offline");
    return;
  }
  _svc.cloud.arm(_cloud_first_post_pending);
  _cloud_first_post_pending = false;
  log_heap(TAG, "wifi.connected:after-cloud-start");
}

void Orchestrator::on_wifi_disconnected(WifiDisconnectReason reason) {
  AG_LOGI(TAG, "wifi disconnected: reason=%u", static_cast<unsigned>(reason));
  log_heap(TAG, "wifi.disconnected:enter");
  if (_mode != OperatingMode::Stationary) {
    return;
  }

  // Disarm before policy routing; skip requested_by_user (own teardown).
  if (reason != WifiDisconnectReason::requested_by_user) {
    _svc.cloud.disarm();
  }

  // Spec disconnect-policy table:
  //   auth_failed                          -> open provisioning (always)
  //   no_ap_found / assoc_failed /
  //   dhcp_failed / connection_lost        -> open provisioning before
  //                                           first IP; stay disconnected
  //                                           after.
  //   ap_disconnected / handshake_failed /
  //   unknown                              -> stay; timeout may synthesize.
  //   requested_by_user                    -> ignore (service teardown).
  const bool before_first_online = !_svc.wifi.has_been_online();
  bool open_provisioning = false;
  switch (reason) {
  case WifiDisconnectReason::auth_failed:
    open_provisioning = true;
    break;
  case WifiDisconnectReason::no_ap_found:
  case WifiDisconnectReason::assoc_failed:
  case WifiDisconnectReason::dhcp_failed:
  case WifiDisconnectReason::connection_lost:
    open_provisioning = before_first_online;
    break;
  case WifiDisconnectReason::ap_disconnected:
  case WifiDisconnectReason::handshake_failed:
  case WifiDisconnectReason::unknown:
  case WifiDisconnectReason::requested_by_user:
    break;
  }

  if (open_provisioning) {
    enter_provisioning_page(ProvisioningTransport::BleOnly);
  }
}

void Orchestrator::on_provisioning_state_changed(const ProvisioningEventPayload &payload) {
  const auto event = static_cast<ProvisioningEvent>(payload.event);
  AG_LOGI(TAG, "provisioning event=%u transport=%u", static_cast<unsigned>(event),
          payload.transport);

  switch (event) {
  case ProvisioningEvent::Started:
    // Transport up; payload carries which one (post-switch update).
    _svc.ui_manager.set_provisioning_transport(
        static_cast<ProvisioningTransport>(payload.transport));
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::WaitingForCredentials);
    update_display();
    break;

  case ProvisioningEvent::Connecting:
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::Connecting);
    update_display();
    break;

  case ProvisioningEvent::ConnectFailed:
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::ConnectFailed);
    update_display();
    break;

  case ProvisioningEvent::Connected:
    _settings.disable_cloud = payload.disable_cloud;
    _settings.static_ip = payload.static_ip;
    save_go_settings(_config_store, _settings);

    _svc.cloud.set_disable_cloud(_settings.disable_cloud);

    // Render "Connected! a.b.c.d" on the Provisioning page first.  The
    // wait=true + flush() pair guarantees the success frame is painted
    // before stop_provisioning()'s internal POST_CONNECT_HOLD_MS (~1.5 s)
    // starts running against the prior frame.
    _svc.ui_manager.set_provisioning_connected(payload.ip);
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::Connected);
    update_display(/*wait=*/true);
    _svc.display_service.flush();

    // Tear down the provisioning transport.  ProvisioningManager::stop()
    // blocks for POST_CONNECT_HOLD_MS (~1.5 s) when called after
    // Connected, which doubles as the on-page hold now that the success
    // page is actually visible.  No snackbar — the page already shows it.
    _svc.wifi.stop_provisioning();

    leave_session_to_home();

    // Provisioning heap freed above; safe to claim cloud task stack now.
    if (!_svc.cloud.start()) {
      AG_LOGE(TAG, "cloud.start() failed; cloud transport offline");
      break;
    }
    _svc.cloud.arm(/*fire_now=*/true);
    break;

  case ProvisioningEvent::Stopped:
    // User abort, timeout, or transport-switch start failure. With no
    // prior online state, fall back to Portable so the device is never
    // stranded on the Provisioning screen with no active transport.
    if (!_svc.wifi.has_been_online()) {
      leave_session_to_portable();
    }
    break;
  }
}

void Orchestrator::pause_provisioning_sensitive_services() {
  if (_provisioning_sensitive_services_paused) {
    return;
  }
  AG_LOGI(TAG, "pausing network-sensitive services");
  _svc.sensor_producer.stop();
  if (is_gps_active()) {
    _svc.gps_service.stop_and_idle_gnss();
  }
  _svc.power_service.set_pm_power(false);
  _provisioning_sensitive_services_paused = true;
  log_heap(TAG, "prov.pause-sensitive:exit");
}

void Orchestrator::resume_provisioning_sensitive_services() {
  if (!_provisioning_sensitive_services_paused) {
    return;
  }
  AG_LOGI(TAG, "resuming network-sensitive services");
  _svc.power_service.set_pm_power(true);
  _svc.sensor_producer.start();
  if (is_gps_active()) {
    _svc.gps_service.start();
  }
  _provisioning_sensitive_services_paused = false;
  // One immediate measurement so the display refreshes promptly after
  // the resume rather than waiting for the next scheduled tick.
  _svc.sensor_producer.request_measurement(1, SensorGroup::All);
  log_heap(TAG, "prov.resume-sensitive:exit");
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void Orchestrator::update_display() { update_display(false); }

void Orchestrator::update_display(bool wait) {
  uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  _svc.ui_manager.clear_expired_snackbar(now_ms);
  BuildContext ctx = build_context();
  DisplayValues values = _svc.ui_manager.build_values(ctx);
  _svc.display_service.update(values, wait);

  // Schedule a follow-up refresh to visually clear the snackbar after it
  // expires.  Only arm once per snackbar — intermediate update_display()
  // calls (from sensor data, input, etc.) that happen before the deadline
  // naturally clear it and the timer fires as a harmless no-op.
  if (values.snackbar_text != nullptr && _snackbar_refresh_deadline_ms == 0) {
    _snackbar_refresh_deadline_ms = now_ms + SNACKBAR_DURATION_MS + 200;
  }

  AG_LOGI(TAG, "display update done");
}

void Orchestrator::request_background_display_update() {
  if (_setup_session_active) {
    // Session screens (Info / Provisioning / ProvisioningConfirm) only
    // re-render on explicit setup state transitions.  Suppressing the
    // background path here prevents sensor / BMS / BLE-status events from
    // racing the orchestrator's deliberate wait=true renders.
    return;
  }
  if (_svc.ui_manager.is_focus_screen()) {
    // Focus screens (PairingPasskey) render constant content while the
    // user is acting on it.  Sensor / BMS events would cause Fast
    // refreshes of an identical frame and eventually trigger the
    // anti-ghost Full refresh.
    return;
  }
  if (!_svc.ui_manager.is_on_menu_screen()) {
    update_display();
  }
}

BuildContext Orchestrator::build_context() const {
  // Convert MeasuresAGo to Measures for the BuildContext reference
  _display_measures = Measures{};
  _display_measures.temp_hum_a = _cached_measures.temp_hum_a;
  _display_measures.pm_a = _cached_measures.pm_a;
  _display_measures.co2 = _cached_measures.co2;
  _display_measures.tvoc_nox = _cached_measures.tvoc_nox;
  _display_measures.power = _cached_measures.power;
  _display_measures.pressure = _cached_measures.pressure;

  // Read chart data cache
  uint16_t cache_count = _svc.storage_service.read_cache(_cache_buf, UI_CHART_BUF_SIZE);

  // Extract battery data
  uint8_t battery_pct = 0xFF;
  if (_latest_power.battery_percentage >= 0.0f) {
    battery_pct = static_cast<uint8_t>(_latest_power.battery_percentage);
  }

  bool is_charging = is_bms_charging(_latest_power.charging_status);

  return BuildContext{
      .sensor_data = _display_measures,
      .battery_pct = battery_pct,
      .is_battery_charging = is_charging,
      .is_plugged_in =
          bms_power_source_has_external_input(_latest_power.charger_status.power_source),
      .locked = (_lock_state == LockState::Locked),
      .ble_enabled = (_mode == OperatingMode::Portable),
      .ble_connected = _svc.ble_service.is_connected(),
      .wifi_enabled = (_mode == OperatingMode::Stationary) && _svc.wifi.is_online(),
      .gps_enabled = is_gps_active(),
      .gps_fix = is_fix_valid(_latest_gps.fix),
      .tracking_active = _tracking_active,
      .display_off = false,
      .use_fahrenheit = _settings.use_fahrenheit,
      .pm_use_usaqi = _settings.pm_use_usaqi,
      .cache = _cache_buf,
      .cache_count = static_cast<uint8_t>(cache_count),
      .now_ms = static_cast<uint32_t>(RTOS::get_time_ms()),
  };
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

void Orchestrator::try_enter_sleep() {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  uint32_t awake_ms = now - _last_measurement_ms;

  auto decision = _svc.power_service.decide_sleep(_settings, _lock_state, _mode, awake_ms);

  if (decision.type == PowerService::SleepType::None) {
    return;
  }

  // decision.type == Deep
  prepare_for_sleep(decision.duration_ms);
  AG_LOGI(TAG, "entering deep sleep (%lu ms)", static_cast<unsigned long>(decision.duration_ms));
  _svc.power_service.enter_sleep(decision.duration_ms);
  // Never returns — CPU reboots on wake.
}

void Orchestrator::prepare_for_sleep(uint32_t sleep_duration_ms) {
  AG_LOGI(TAG, "prepare_for_sleep");
  log_heap(TAG, "sleep.prepare:enter");

  // Ensure pending display refresh completes before stopping worker
  _svc.ui_manager.clear_expired_snackbar(static_cast<uint32_t>(RTOS::get_time_ms()));
  BuildContext ctx = build_context();
  DisplayValues values = _svc.ui_manager.build_values(ctx);
  _svc.display_service.update(values, true); // wait = true

  // Save RTC display snapshot so the next button wake can render immediately
  // without NVS or sensor reads.
  save_rtc_display_snapshot(values);

  _svc.ble_service.deinit();
  _svc.sensor_producer.stop();

  // Active GPS: stop task only — leave TAU1113 tracking for hot-start.
  // Inactive GPS: stop task and send GNSS stop before sleep.
  if (is_gps_active()) {
    _svc.gps_service.stop();
  } else {
    _svc.gps_service.stop_and_idle_gnss();
  }

  _svc.input_service.stop();
  _svc.display_service.stop();

  // Put SSD1680 into deep sleep mode 1 after stopping the worker task.
  // Reduces quiescent current from ~100 µA to <1 µA during ESP deep sleep.
  // driver_hw_init_full() exits deep sleep on next init() via hardware reset.
  _svc.display_service.deep_sleep();

  // Flush and close the route file so buffered route points are committed to
  // NAND before the CPU reboots.  On the next wake, start_route() reopens the
  // same file in append mode and restores the point count from its size.
  // No-op when no route is active.
  _svc.storage_service.end_route();

  _svc.storage_service.backup_cache();

  // Persist RTC state with the warm-sensor flag for the next wake cycle.
  RtcAppState state = snapshot_state();
  state.sensors_warm = _svc.power_service.should_hold_pm_sensor(sleep_duration_ms);
  _svc.power_service.save_state(state);

  // Reset external watchdog last — gives it the full timeout window during sleep.
  _svc.power_service.reset_ext_watchdog();

  // Start LP Core to keep pulsing the external watchdog during deep sleep.
  ulp_wdt_start();
  log_heap(TAG, "sleep.prepare:before-sleep");
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void Orchestrator::deactivate_gps() {
  _svc.gps_service.stop_and_idle_gnss();
  _latest_gps = GpsData{};
}

bool Orchestrator::is_gps_active() const {
  if (_settings.gps_mode == GpsMode::AlwaysOff) {
    return false;
  }
  if (_settings.gps_mode == GpsMode::AlwaysOn) {
    return true;
  }
  // GpsMode::OnWhenTracking
  return _tracking_active;
}

uint32_t Orchestrator::generate_session_id() {
  const uint32_t new_id = generate_random_number(SESSION_ID_LENGTH);
  AG_LOGI(TAG, "generate_session_id: %" PRIu32, new_id);
  return new_id;
}

RtcAppState Orchestrator::snapshot_state() const {
  return RtcAppState{
      .mode = _mode,
      .behavior = _behavior,
      .lock_state = _lock_state,
      .gps_enabled = _gps_enabled,
      .tracking_active = _tracking_active,
      .tracking_session_id = _tracking_session_id,
  };
}
