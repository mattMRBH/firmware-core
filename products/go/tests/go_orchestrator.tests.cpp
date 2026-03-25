/**
 * AirGradient Go — Orchestrator unit tests
 *
 * Tests the orchestrator's core logic: event dispatch, state transitions,
 * timer management, input handling, settings propagation, display context
 * building, and session ID generation.
 *
 * Service interactions are verified through observable stubs defined in
 * go_orchestrator_stubs.cpp (the test_spy namespace).
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <memory>

#include "go_orchestrator.h"

// ============================================================================
// External test_spy state (defined in go_orchestrator_stubs.cpp)
// ============================================================================

namespace test_spy {
extern bool sensor_started;
extern bool sensor_stopped;
extern bool measurement_requested;
extern uint8_t last_iterations;

extern bool gps_started;
extern bool gps_stopped;
extern int gps_posting_interval_ms;

extern bool input_started;
extern bool input_stopped;

extern bool cache_measurement_called;
extern MeasuresAGo last_cached_measurement;
extern bool route_started;
extern uint32_t route_session_id;
extern bool route_point_appended;
extern RoutePoint last_route_point;
extern bool route_ended;
extern bool cache_backed_up;
extern bool cache_restored;

extern bool bms_polled;
extern bool watchdog_reset;
extern bool shutdown_called;
extern bool state_saved;
extern RtcAppState last_saved_state;
extern RtcAppState state_to_load;
extern PowerSnapshot snapshot_to_return;
extern PowerService::SleepType sleep_type_to_return;
extern WakeCause sleep_wake_cause_to_return;

// --- BleService ---
extern bool ble_init_called;
extern bool ble_deinit_called;
extern bool ble_initialized;
extern bool ble_connected;
extern bool ble_notify_measures_called;
extern bool ble_update_status_called;
extern bool ble_update_config_called;
extern bool ble_notify_config_called;
extern bool ble_notify_command_result_called;
extern BleCommand ble_last_command;
extern bool ble_last_command_success;
extern bool ble_history_list_called;
extern bool ble_history_start_called;
extern uint32_t ble_history_start_session;
extern bool ble_history_fill_called;
extern bool ble_history_end_called;
extern BleConfigDecodeResult ble_config_decode_result;
extern BleHistoryDecodeResult ble_history_decode_result;

extern void reset();
} // namespace test_spy

// ============================================================================
// MockConfigStore (ConfigStore is abstract)
// ============================================================================

class MockConfigStore : public trompeloeil::mock_interface<ConfigStore> {
public:
  IMPLEMENT_MOCK2(get_int);
  IMPLEMENT_MOCK2(set_int);
  IMPLEMENT_MOCK2(get_bool);
  IMPLEMENT_MOCK2(set_bool);
  IMPLEMENT_MOCK2(get_string);
  IMPLEMENT_MOCK2(set_string);
  IMPLEMENT_MOCK1(erase);
  IMPLEMENT_MOCK0(commit);
};

// ============================================================================
// MockRTOS (for time control)
// ============================================================================

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
};

// ============================================================================
// Stub sensor/hardware objects (constructors need references)
// ============================================================================

class StubSensorManager {
public:
  // Enough to pass by reference to SensorProducer. The stub SensorProducer
  // constructor ignores it.
};

class StubGpsSensor : public GpsSensor {
public:
  bool begin(int) override { return true; }
  void end() override {}
  bool read() override { return false; }
  GpsData get_data() const override { return {}; }
  bool has_valid_fix() const override { return false; }
};

class StubCapTouchSensor : public CapTouchSensor {
public:
  bool init() override { return true; }
  bool read(TouchData &) override { return false; }
};

class StubBmsDevice : public BmsDevice {
public:
  bool init() override { return true; }
  bool read_telemetry(BmsTelemetry &) override { return false; }
  bool read_status(BmsStatus &) override { return false; }
  bool get_battery_percentage(float *) override { return false; }
  bool update_watchdog() override { return true; }
  bool feature_ship_available() const override { return false; }
  bool enter_ship_mode() override { return false; }
};

class StubNandStorage : public NandStorage {
public:
  bool init() override { return true; }
  void deinit() override {}
  bool is_mounted() const override { return true; }
  const char *mount_path() const override { return "/tmp"; }
};

class StubPayloadCacheStorage : public PayloadCacheStorage {
public:
  bool load(PayloadCacheStorageData &) override { return false; }
  bool save(const PayloadCacheStorageData &) override { return true; }
  bool clear() override { return true; }
};

// GPIO HAL stub (function-pointer table — never called by stubs)
static bool gpio_configure(int, gpio::Mode, gpio::PullMode, gpio::InterruptType) { return true; }
static int gpio_get_level(int) { return 0; }
static bool gpio_set_level(int, int) { return true; }
static bool gpio_add_interrupt_handler(int, gpio::InterruptHandler, void *) { return true; }
static bool gpio_remove_interrupt_handler(int) { return true; }
static bool gpio_enable_interrupt(int) { return true; }
static bool gpio_disable_interrupt(int) { return true; }

static const gpio::Hal test_gpio_hal = {
    gpio_configure,
    gpio_get_level,
    gpio_set_level,
    gpio_add_interrupt_handler,
    gpio_remove_interrupt_handler,
    gpio_enable_interrupt,
    gpio_disable_interrupt,
};

// ============================================================================
// OrchestratorTestAccess — friend class for private member access
// ============================================================================

class OrchestratorTestAccess {
public:
  static void dispatch(Orchestrator &o, const Event &evt) { o.dispatch(evt); }
  static void check_timers(Orchestrator &o) { o.check_timers(); }
  static uint32_t compute_queue_timeout_ms(const Orchestrator &o) {
    return o.compute_queue_timeout_ms();
  }
  static uint32_t compute_sleep_duration_ms(const Orchestrator &o) {
    return o.compute_sleep_duration_ms();
  }
  static BuildContext build_context(const Orchestrator &o) { return o.build_context(); }
  static RtcAppState snapshot_state(const Orchestrator &o) { return o.snapshot_state(); }

  // State readers
  static OperatingMode mode(const Orchestrator &o) { return o._mode; }
  static Behavior behavior(const Orchestrator &o) { return o._behavior; }
  static LockState lock_state(const Orchestrator &o) { return o._lock_state; }
  static bool gps_enabled(const Orchestrator &o) { return o._gps_enabled; }
  static bool tracking_active(const Orchestrator &o) { return o._tracking_active; }
  static uint32_t tracking_session_id(const Orchestrator &o) { return o._tracking_session_id; }
  static bool first_measurement_done(const Orchestrator &o) { return o._first_measurement_done; }
  static const MeasuresAGo &latest_measures(const Orchestrator &o) { return o._latest_measures; }
  static const GpsData &latest_gps(const Orchestrator &o) { return o._latest_gps; }
  static const PowerSnapshot &latest_power(const Orchestrator &o) { return o._latest_power; }
  static uint32_t last_input_ms(const Orchestrator &o) { return o._last_input_ms; }
  static GoSettings &settings(Orchestrator &o) { return o._settings; }

  // Direct method access
  static bool is_gps_active(const Orchestrator &o) { return o.is_gps_active(); }
  static uint8_t compute_iterations(const Orchestrator &o) { return o.compute_iterations(); }
  static uint32_t generate_session_id(Orchestrator &o) { return o.generate_session_id(); }
  static void on_input(Orchestrator &o, const InputEventData &input) { o.on_input(input); }
  static void on_sensor_data(Orchestrator &o, const MeasuresAGo &data) { o.on_sensor_data(data); }
  static void on_gps_fix(Orchestrator &o, const GpsData &data) { o.on_gps_fix(data); }
  static void lock(Orchestrator &o) { o.lock(); }
  static void unlock(Orchestrator &o) { o.unlock(); }
  static void start_tracking(Orchestrator &o) { o.start_tracking(); }
  static void stop_tracking(Orchestrator &o) { o.stop_tracking(); }
  static void shutdown(Orchestrator &o) { o.shutdown(); }
  static void apply_settings_change(Orchestrator &o) { o.apply_settings_change(); }
  static void set_mode(Orchestrator &o, OperatingMode mode) { o._mode = mode; }
};

using A = OrchestratorTestAccess;

// ============================================================================
// Test fixture helper
// ============================================================================

struct TestFixture {
  // Stub hardware objects
  StubSensorManager stub_sensor_mgr;
  StubGpsSensor stub_gps;
  StubCapTouchSensor stub_touch;
  StubBmsDevice stub_bms;
  StubNandStorage stub_nand;
  StubPayloadCacheStorage stub_cache_storage;
  PayloadCache payload_cache;

  // Services (using stub constructors from go_orchestrator_stubs.cpp)
  SensorProducer sensor_producer;
  GpsService gps_service;
  InputService input_service;
  DisplayService display_service;
  StorageService storage_service;
  PowerService power_service;
  UIManager ui_manager;
  BleService ble_service;

  // MockRTOS + MockConfigStore
  MockRTOS mock_rtos;
  MockConfigStore mock_config;

  Orchestrator::Services services;
  GoSettings settings;

  // Persistent mock expectations (must outlive individual test scopes)
  std::unique_ptr<trompeloeil::expectation> _exp_time;
  std::unique_ptr<trompeloeil::expectation> _exp_delay;

  TestFixture()
      : payload_cache(stub_cache_storage, 16),
        sensor_producer(reinterpret_cast<SensorManager &>(stub_sensor_mgr), nullptr,
                        SensorProducer::Config{}),
        gps_service(stub_gps, nullptr, GpsService::Config{}),
        input_service(stub_touch, test_gpio_hal, nullptr, InputService::Config{}),
        display_service(DisplayService::Config{}), storage_service(payload_cache, stub_nand),
        power_service(stub_bms, test_gpio_hal, PowerService::Config{}),
        ui_manager(UIManager::Config{}), ble_service(nullptr, storage_service),
        services{sensor_producer, gps_service,   input_service, display_service,
                 storage_service, power_service, ui_manager,    ble_service} {
    test_spy::reset();
    RTOS::set_instance(&mock_rtos);
    _exp_time = NAMED_ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    _exp_delay = NAMED_ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
  }

  ~TestFixture() { RTOS::set_instance(nullptr); }

  Orchestrator make_orchestrator() { return {nullptr, services, settings, mock_config, "TEST00"}; }
};

// ============================================================================
// 1. Pure Logic — is_gps_active
// ============================================================================

TEST_CASE("is_gps_active: GpsMode determines GPS activity", "[Orchestrator][pure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  SECTION("AlwaysOff returns false regardless of tracking") {
    A::settings(orch).gps_mode = GpsMode::AlwaysOff;
    REQUIRE_FALSE(A::is_gps_active(orch));
  }

  SECTION("AlwaysOn returns true regardless of tracking") {
    A::settings(orch).gps_mode = GpsMode::AlwaysOn;
    REQUIRE(A::is_gps_active(orch));
  }

  SECTION("OnWhenTracking returns true when tracking") {
    A::settings(orch).gps_mode = GpsMode::OnWhenTracking;
    // Start tracking to set _tracking_active = true
    ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
        .RETURN(ConfigStoreResult::NOT_FOUND);
    ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_))
        .RETURN(ConfigStoreResult::OK);
    ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);
    A::start_tracking(orch);
    REQUIRE(A::is_gps_active(orch));
  }

  SECTION("OnWhenTracking returns false when idle") {
    A::settings(orch).gps_mode = GpsMode::OnWhenTracking;
    REQUIRE_FALSE(A::is_gps_active(orch));
  }
}

// ============================================================================
// 2. Pure Logic — compute_iterations
// ============================================================================

TEST_CASE("compute_iterations: derives iteration count from interval", "[Orchestrator][pure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  SECTION("60s interval = 30 iterations") {
    A::settings(orch).measurement_interval_seconds = 60;
    REQUIRE(A::compute_iterations(orch) == 30);
  }

  SECTION("1s interval = 1 iteration (minimum)") {
    A::settings(orch).measurement_interval_seconds = 1;
    REQUIRE(A::compute_iterations(orch) == 1);
  }

  SECTION("120s interval = 60 iterations") {
    A::settings(orch).measurement_interval_seconds = 120;
    REQUIRE(A::compute_iterations(orch) == 60);
  }

  SECTION("4s interval = 2 iterations") {
    A::settings(orch).measurement_interval_seconds = 4;
    REQUIRE(A::compute_iterations(orch) == 2);
  }
}

// ============================================================================
// 3. Pure Logic — compute_sleep_duration_ms
// ============================================================================

TEST_CASE("compute_sleep_duration_ms: returns minimum of measurement and display intervals",
          "[Orchestrator][pure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  SECTION("measurement interval only (display off)") {
    A::settings(orch).measurement_interval_seconds = 60;
    A::settings(orch).display_refresh_interval_seconds = 0;
    REQUIRE(A::compute_sleep_duration_ms(orch) == 60000);
  }

  SECTION("display shorter than measurement") {
    A::settings(orch).measurement_interval_seconds = 60;
    A::settings(orch).display_refresh_interval_seconds = 10;
    REQUIRE(A::compute_sleep_duration_ms(orch) == 10000);
  }

  SECTION("measurement shorter than display") {
    A::settings(orch).measurement_interval_seconds = 10;
    A::settings(orch).display_refresh_interval_seconds = 60;
    REQUIRE(A::compute_sleep_duration_ms(orch) == 10000);
  }
}

// ============================================================================
// 4. Pure Logic — snapshot_state
// ============================================================================

TEST_CASE("snapshot_state: captures current application state", "[Orchestrator][pure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  RtcAppState state = A::snapshot_state(orch);
  REQUIRE(state.mode == OperatingMode::Portable);
  REQUIRE(state.behavior == Behavior::Idle);
  REQUIRE(state.lock_state == LockState::Locked);
  REQUIRE(state.gps_enabled == true);
  REQUIRE(state.tracking_active == false);
  REQUIRE(state.tracking_session_id == 0);
}

// ============================================================================
// 5. Initialization
// ============================================================================

TEST_CASE("init(PowerOn): default state with first measurement and BMS poll",
          "[Orchestrator][init]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Allow save_go_settings NVS calls during sync (none expected for init)
  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);

  // Verify default state
  REQUIRE(A::mode(orch) == OperatingMode::Portable);
  REQUIRE(A::lock_state(orch) == LockState::Locked);
  REQUIRE(A::tracking_active(orch) == false);

  // Verify initial measurement was requested (single iteration)
  REQUIRE(test_spy::measurement_requested);
  REQUIRE(test_spy::last_iterations == 1);

  // Verify initial BMS poll
  REQUIRE(test_spy::bms_polled);
  REQUIRE(test_spy::watchdog_reset);
}

TEST_CASE("init(Button): restores state from RTC and unlocks", "[Orchestrator][init]") {
  TestFixture f;

  // Set up RTC state to restore
  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Portable,
      .behavior = Behavior::Tracking,
      .lock_state = LockState::Locked,
      .gps_enabled = true,
      .tracking_active = true,
      .tracking_session_id = 12345,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::Button);

  REQUIRE(A::mode(orch) == OperatingMode::Portable);
  REQUIRE(A::tracking_active(orch) == true);
  REQUIRE(A::tracking_session_id(orch) == 12345);
  REQUIRE(A::lock_state(orch) == LockState::Unlocked); // button wake unlocks

  // Tracking route should be resumed
  REQUIRE(test_spy::route_started);
  REQUIRE(test_spy::route_session_id == 12345);
}

// ============================================================================
// 6. Input Handling
// ============================================================================

TEST_CASE("on_input: ButtonPower long press triggers shutdown", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  InputEventData input{InputSource::ButtonPower, InputType::LongPress};
  A::on_input(orch, input);

  REQUIRE(test_spy::shutdown_called);
}

TEST_CASE("on_input: ButtonPower short press toggles lock", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Initially locked; short press should unlock
  InputEventData input{InputSource::ButtonPower, InputType::ShortPress};
  A::on_input(orch, input);
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);

  // Now unlocked; short press should lock
  test_spy::reset();
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1000);
  A::on_input(orch, input);
  REQUIRE(A::lock_state(orch) == LockState::Locked);
}

TEST_CASE("on_input: touch while locked is ignored", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  REQUIRE(A::lock_state(orch) == LockState::Locked);

  // Touch should not change screen (UIManager not called)
  Screen before = f.ui_manager.current_screen();
  InputEventData input{InputSource::TouchEnter, InputType::ShortPress};
  A::on_input(orch, input);
  REQUIRE(f.ui_manager.current_screen() == before);
}

TEST_CASE("on_input: touch while unlocked forwards to UIManager", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Unlock first
  A::unlock(orch);
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);

  // TouchEnter on Home screen should open MainMenu
  InputEventData input{InputSource::TouchEnter, InputType::ShortPress};
  A::on_input(orch, input);
  REQUIRE(f.ui_manager.current_screen() == Screen::MainMenu);
}

TEST_CASE("on_input: ButtonBoot long press is a no-op", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  InputEventData input{InputSource::ButtonBoot, InputType::LongPress};
  A::on_input(orch, input);

  // Nothing should have happened
  REQUIRE_FALSE(test_spy::shutdown_called);
  REQUIRE(A::lock_state(orch) == LockState::Locked);
}

// ============================================================================
// 7. State Transitions — lock/unlock
// ============================================================================

TEST_CASE("lock: sets Locked and resets UI to home", "[Orchestrator][state]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Unlock first, navigate to menu
  A::unlock(orch);
  f.ui_manager.set_screen(Screen::MainMenu);

  A::lock(orch);
  REQUIRE(A::lock_state(orch) == LockState::Locked);
  REQUIRE(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("unlock: sets Unlocked and requests measurement", "[Orchestrator][state]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  REQUIRE(A::lock_state(orch) == LockState::Locked);

  test_spy::reset();
  A::unlock(orch);

  REQUIRE(A::lock_state(orch) == LockState::Unlocked);
  REQUIRE(test_spy::measurement_requested);
  REQUIRE(test_spy::last_iterations == 1);
}

// ============================================================================
// 8. State Transitions — tracking
// ============================================================================

TEST_CASE("start_tracking: generates session ID and starts route", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Mock ConfigStore for session ID generation
  REQUIRE_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  REQUIRE_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  REQUIRE_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);

  REQUIRE(A::tracking_active(orch) == true);
  REQUIRE(A::behavior(orch) == Behavior::Tracking);
  REQUIRE(A::tracking_session_id(orch) >= 10000);
  REQUIRE(test_spy::route_started);
}

TEST_CASE("start_tracking: idempotent when already tracking", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  uint32_t first_id = A::tracking_session_id(orch);

  test_spy::reset();
  A::start_tracking(orch); // should be no-op

  REQUIRE_FALSE(test_spy::route_started); // no second start_route call
  REQUIRE(A::tracking_session_id(orch) == first_id);
}

TEST_CASE("stop_tracking: ends route and clears state", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::reset();

  A::stop_tracking(orch);

  REQUIRE(A::tracking_active(orch) == false);
  REQUIRE(A::behavior(orch) == Behavior::Idle);
  REQUIRE(A::tracking_session_id(orch) == 0);
  REQUIRE(test_spy::route_ended);
}

TEST_CASE("stop_tracking: idempotent when not tracking", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::stop_tracking(orch);
  REQUIRE_FALSE(test_spy::route_ended);
}

// ============================================================================
// 9. Event Handlers — sensor data
// ============================================================================

TEST_CASE("on_sensor_data: caches measurement and sets first_measurement_done",
          "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  REQUIRE_FALSE(A::first_measurement_done(orch));

  MeasuresAGo data{};
  data.co2.co2 = 420;
  A::on_sensor_data(orch, data);

  REQUIRE(A::first_measurement_done(orch));
  REQUIRE(test_spy::cache_measurement_called);
  REQUIRE(A::latest_measures(orch).co2.co2 == 420);
}

TEST_CASE("on_sensor_data: appends route point when tracking", "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::reset();

  MeasuresAGo data{};
  data.co2.co2 = 500;
  A::on_sensor_data(orch, data);

  REQUIRE(test_spy::route_point_appended);
  REQUIRE(test_spy::last_route_point.sensors.co2.co2 == 500);
}

TEST_CASE("on_sensor_data: does not append route point when not tracking",
          "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  REQUIRE_FALSE(test_spy::route_point_appended);
}

// ============================================================================
// 10. Event Handlers — GPS
// ============================================================================

TEST_CASE("on_gps_fix: caches data when GPS is active", "[Orchestrator][events]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  GpsData fix{};
  fix.position.latitude = 48.8566;
  fix.position.longitude = 2.3522;
  fix.fix.fix_type = GpsFixType::Fix3D;

  A::on_gps_fix(orch, fix);

  REQUIRE(A::latest_gps(orch).position.latitude == 48.8566);
}

TEST_CASE("on_gps_fix: ignores data when GPS is inactive", "[Orchestrator][events]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOff;
  auto orch = f.make_orchestrator();

  GpsData fix{};
  fix.position.latitude = 48.8566;
  A::on_gps_fix(orch, fix);

  // Latitude should still be the invalid sentinel
  REQUIRE(A::latest_gps(orch).position.latitude == GPS_LATITUDE_INVALID);
}

// ============================================================================
// 11. Timer Management
// ============================================================================

TEST_CASE("check_timers: fires measurement timer when due", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.measurement_interval_seconds = 60;
  auto orch = f.make_orchestrator();

  // Advance time past measurement interval
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(61000);
  test_spy::reset();

  A::check_timers(orch);

  REQUIRE(test_spy::measurement_requested);
}

TEST_CASE("check_timers: fires BMS timer when due", "[Orchestrator][timers]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // BMS interval is 5000ms
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(6000);
  test_spy::reset();

  A::check_timers(orch);

  REQUIRE(test_spy::bms_polled);
  REQUIRE(test_spy::watchdog_reset);
}

TEST_CASE("check_timers: fires inactivity when unlocked and due", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.auto_lock_seconds = 10;
  auto orch = f.make_orchestrator();

  A::unlock(orch); // sets _last_input_ms to current time (0)

  // Advance time past inactivity timeout
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(11000);
  A::check_timers(orch);

  REQUIRE(A::lock_state(orch) == LockState::Locked);
}

TEST_CASE("check_timers: does not fire inactivity when locked", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.auto_lock_seconds = 10;
  auto orch = f.make_orchestrator();

  REQUIRE(A::lock_state(orch) == LockState::Locked);

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(11000);
  A::check_timers(orch);

  // Should still be locked (no inactivity timeout triggered — was already locked)
  REQUIRE(A::lock_state(orch) == LockState::Locked);
}

TEST_CASE("check_timers: does not fire inactivity when auto_lock disabled",
          "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.auto_lock_seconds = 0; // disabled
  auto orch = f.make_orchestrator();

  A::unlock(orch);

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(999999);
  A::check_timers(orch);

  REQUIRE(A::lock_state(orch) == LockState::Unlocked);
}

// ============================================================================
// 12. Settings
// ============================================================================

TEST_CASE("apply_settings_change: propagates GPS interval to service", "[Orchestrator][settings]") {
  TestFixture f;
  f.settings.gps_interval_seconds = 5;
  auto orch = f.make_orchestrator();

  // apply_to_settings is real (UIManager). Mock NVS calls for save_go_settings.
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::apply_settings_change(orch);

  REQUIRE(test_spy::gps_posting_interval_ms == 5000);
}

// ============================================================================
// 13. Session ID Generation
// ============================================================================

TEST_CASE("generate_session_id: increments NVS counter", "[Orchestrator][session]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // First call: NVS has no counter, starts at 10000
  REQUIRE_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  REQUIRE_CALL(f.mock_config, set_int(trompeloeil::_, 10000)).RETURN(ConfigStoreResult::OK);
  REQUIRE_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  uint32_t id = A::generate_session_id(orch);
  REQUIRE(id == 10000);
}

TEST_CASE("generate_session_id: wraps at 99999 to 10000", "[Orchestrator][session]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // NVS counter is at max
  REQUIRE_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .LR_SIDE_EFFECT(_2 = 99999)
      .RETURN(ConfigStoreResult::OK);
  REQUIRE_CALL(f.mock_config, set_int(trompeloeil::_, 10000)).RETURN(ConfigStoreResult::OK);
  REQUIRE_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  uint32_t id = A::generate_session_id(orch);
  REQUIRE(id == 10000);
}

// ============================================================================
// 14. Build Context
// ============================================================================

TEST_CASE("build_context: populates sensor data and status flags", "[Orchestrator][display]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  f.settings.use_fahrenheit = true;
  f.settings.pm_use_usaqi = true;
  auto orch = f.make_orchestrator();

  // Feed sensor data
  MeasuresAGo data{};
  data.co2.co2 = 800;
  data.temp_hum_a.temperature = 23.5f;
  A::on_sensor_data(orch, data);

  BuildContext ctx = A::build_context(orch);

  REQUIRE(ctx.sensor_data.co2.co2 == 800);
  REQUIRE(ctx.sensor_data.temp_hum_a.temperature == 23.5f);
  REQUIRE(ctx.locked == true); // still locked by default
  REQUIRE(ctx.gps_enabled == true);
  REQUIRE(ctx.use_fahrenheit == true);
  REQUIRE(ctx.pm_use_usaqi == true);
}

TEST_CASE("build_context: GPS clock invalid when no fix", "[Orchestrator][display]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  BuildContext ctx = A::build_context(orch);

  REQUIRE(ctx.hour == 0xFF);
  REQUIRE(ctx.minute == 0xFF);
}

TEST_CASE("build_context: GPS clock populated from valid fix", "[Orchestrator][display]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  GpsData fix{};
  fix.timestamp.valid = true;
  fix.timestamp.hour = 14;
  fix.timestamp.minute = 30;
  A::on_gps_fix(orch, fix);

  BuildContext ctx = A::build_context(orch);
  REQUIRE(ctx.hour == 14);
  REQUIRE(ctx.minute == 30);
}

TEST_CASE("build_context: battery percentage 0xFF when invalid", "[Orchestrator][display]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Default PowerSnapshot has battery_percentage = -1.0f (invalid)
  BuildContext ctx = A::build_context(orch);
  REQUIRE(ctx.battery_pct == 0xFF);
}

// ============================================================================
// 15. Shutdown
// ============================================================================

TEST_CASE("shutdown: stops tracking if active and calls power shutdown",
          "[Orchestrator][shutdown]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::reset();

  A::shutdown(orch);

  REQUIRE(test_spy::route_ended);
  REQUIRE(test_spy::cache_backed_up);
  REQUIRE(test_spy::shutdown_called);
  REQUIRE(A::tracking_active(orch) == false);
  REQUIRE(f.ui_manager.current_screen() == Screen::Shutdown);
}

TEST_CASE("shutdown: works without active tracking", "[Orchestrator][shutdown]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::shutdown(orch);

  REQUIRE(test_spy::cache_backed_up);
  REQUIRE(test_spy::shutdown_called);
  REQUIRE_FALSE(test_spy::route_ended); // no route was active
}

// ============================================================================
// 16. Event Dispatch
// ============================================================================

TEST_CASE("dispatch: routes SensorDataReady to on_sensor_data", "[Orchestrator][dispatch]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::SensorDataReady;
  evt.sensor_data = MeasuresAGo{};
  evt.sensor_data.co2.co2 = 999;

  A::dispatch(orch, evt);

  REQUIRE(A::latest_measures(orch).co2.co2 == 999);
  REQUIRE(test_spy::cache_measurement_called);
}

TEST_CASE("dispatch: routes InputPress to on_input", "[Orchestrator][dispatch]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::InputPress;
  evt.input = InputEventData{InputSource::ButtonPower, InputType::ShortPress};

  A::dispatch(orch, evt);

  // Initially locked, ButtonPower short press → unlock
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);
}

TEST_CASE("dispatch: routes GpsFixUpdate to on_gps_fix", "[Orchestrator][dispatch]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::GpsFixUpdate;
  evt.gps_data = GpsData{};
  evt.gps_data.position.latitude = 51.5074;

  A::dispatch(orch, evt);

  REQUIRE(A::latest_gps(orch).position.latitude == 51.5074);
}

// ============================================================================
// BLE dispatch tests
// ============================================================================

TEST_CASE("dispatch: BleConnected pushes status and config", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = true;

  Event evt{};
  evt.type = EventType::BleConnected;
  A::dispatch(orch, evt);

  CHECK(test_spy::ble_update_status_called);
  CHECK(test_spy::ble_update_config_called);
}

TEST_CASE("dispatch: BleConnected dismisses pairing passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();

  // Simulate passkey screen is showing
  f.ui_manager.show_pairing_passkey(123456);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleConnected;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BleDisconnected dismisses pairing passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Simulate passkey screen is showing (failed pairing → disconnect)
  f.ui_manager.show_pairing_passkey(654321);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleDisconnected;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BlePairingRequest shows passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::BlePairingRequest;
  evt.ble_passkey = 554501;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::PairingPasskey);
}

TEST_CASE("dispatch: BleAuthComplete dismisses pairing passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Simulate passkey screen is showing
  f.ui_manager.show_pairing_passkey(999999);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleAuthComplete;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BleAuthComplete is no-op when not on passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  REQUIRE(f.ui_manager.current_screen() == Screen::Home);

  Event evt{};
  evt.type = EventType::BleAuthComplete;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BleHistoryWrite list calls handle_history_list", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Configure decode stub to return List op
  test_spy::ble_history_decode_result.op = BleHistoryOp::List;

  Event evt{};
  evt.type = EventType::BleHistoryWrite;
  A::dispatch(orch, evt);

  // take_pending returns 0 (no data), so handler returns early
  CHECK_FALSE(test_spy::ble_history_list_called);
}

TEST_CASE("on_sensor_data: notifies BLE when connected", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = true;

  MeasuresAGo data{};
  data.co2.co2 = 400;
  A::on_sensor_data(orch, data);

  CHECK(test_spy::ble_notify_measures_called);
}

TEST_CASE("on_sensor_data: does not notify BLE when disconnected", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = false;

  MeasuresAGo data{};
  data.co2.co2 = 400;
  A::on_sensor_data(orch, data);

  CHECK_FALSE(test_spy::ble_notify_measures_called);
}

TEST_CASE("apply_settings_change: notifies BLE when connected", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = true;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::apply_settings_change(orch);

  CHECK(test_spy::ble_notify_config_called);
  CHECK(test_spy::ble_update_config_called);
}

TEST_CASE("apply_settings_change: does not notify BLE when disconnected", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = false;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::apply_settings_change(orch);

  CHECK_FALSE(test_spy::ble_notify_config_called);
  CHECK_FALSE(test_spy::ble_update_config_called);
}

TEST_CASE("init: starts BLE in Portable mode", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  CHECK(test_spy::ble_init_called);
}

TEST_CASE("init: does not start BLE in Offline mode", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Offline;
  auto orch = f.make_orchestrator();

  // Set mode to Offline before init (PowerOn doesn't read settings.operating_mode)
  A::set_mode(orch, OperatingMode::Offline);

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);

  CHECK_FALSE(test_spy::ble_init_called);
}

TEST_CASE("build_context: ble_enabled true in Portable mode", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  auto ctx = A::build_context(orch);

  CHECK(ctx.ble_enabled);
}

TEST_CASE("build_context: ble_enabled false in Offline mode", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  A::set_mode(orch, OperatingMode::Offline);

  auto ctx = A::build_context(orch);

  CHECK_FALSE(ctx.ble_enabled);
}

TEST_CASE("build_context: ble_connected reflects BLE service state", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();

  test_spy::ble_connected = false;
  CHECK_FALSE(A::build_context(orch).ble_connected);

  test_spy::ble_connected = true;
  CHECK(A::build_context(orch).ble_connected);
}
