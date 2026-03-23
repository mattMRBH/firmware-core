/**
 * AirGradient Go — Orchestrator test stubs
 *
 * Provides observable stub implementations for all services the orchestrator
 * calls.  Each stub records its calls into the test_spy namespace so tests
 * can assert on orchestrator–service interactions without constructing real
 * hardware-backed service objects.
 *
 * The stubs replace the real .cpp files at link time — only the orchestrator
 * .cpp (the code under test) and UIManager .cpp (real, pure-logic) are
 * compiled from production sources.
 */

#include "go_gps.h"
#include "go_input.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_storage.h"

#include <algorithm>
#include <cstring>

// ============================================================================
// test_spy — observable state written by stubs, read by test assertions
// ============================================================================

namespace test_spy {

// --- SensorProducer ---
bool sensor_started = false;
bool sensor_stopped = false;
bool measurement_requested = false;
uint8_t last_iterations = 0;

// --- GpsService ---
bool gps_started = false;
bool gps_stopped = false;
int gps_posting_interval_ms = 0;

// --- InputService ---
bool input_started = false;
bool input_stopped = false;

// --- StorageService ---
bool cache_measurement_called = false;
MeasuresAGo last_cached_measurement{};
bool route_started = false;
uint32_t route_session_id = 0;
bool route_point_appended = false;
RoutePoint last_route_point{};
bool route_ended = false;
bool cache_backed_up = false;
bool cache_restored = false;

// --- PowerService ---
bool bms_polled = false;
bool watchdog_reset = false;
bool shutdown_called = false;
bool state_saved = false;
RtcAppState last_saved_state{};
RtcAppState state_to_load{};        // tests set this before init(Button)
PowerSnapshot snapshot_to_return{}; // tests set this before poll_bms
PowerService::SleepType sleep_type_to_return = PowerService::SleepType::None;
WakeCause sleep_wake_cause_to_return = WakeCause::PowerOn;

void reset() {
  sensor_started = false;
  sensor_stopped = false;
  measurement_requested = false;
  last_iterations = 0;

  gps_started = false;
  gps_stopped = false;
  gps_posting_interval_ms = 0;

  input_started = false;
  input_stopped = false;

  cache_measurement_called = false;
  last_cached_measurement = MeasuresAGo{};
  route_started = false;
  route_session_id = 0;
  route_point_appended = false;
  last_route_point = RoutePoint{};
  route_ended = false;
  cache_backed_up = false;
  cache_restored = false;

  bms_polled = false;
  watchdog_reset = false;
  shutdown_called = false;
  state_saved = false;
  last_saved_state = RtcAppState{};
  state_to_load = RtcAppState{};
  snapshot_to_return = PowerSnapshot{};
  sleep_type_to_return = PowerService::SleepType::None;
  sleep_wake_cause_to_return = WakeCause::PowerOn;
}

} // namespace test_spy

// ============================================================================
// SensorProducer stubs
// ============================================================================

SensorProducer::SensorProducer(SensorManager &manager, RtosQueueHandle event_queue,
                               const Config &config)
    : _manager(manager), _event_queue(event_queue), _config(config) {}

bool SensorProducer::start() {
  test_spy::sensor_started = true;
  return true;
}

void SensorProducer::stop() { test_spy::sensor_stopped = true; }

void SensorProducer::request_measurement(uint8_t iterations) {
  test_spy::measurement_requested = true;
  test_spy::last_iterations = iterations;
}

// ============================================================================
// GpsService stubs
// ============================================================================

GpsService::GpsService(GpsSensor &gps, RtosQueueHandle event_queue, const Config &config)
    : _gps(gps), _event_queue(event_queue), _config(config) {}

GpsService::~GpsService() = default;

bool GpsService::start() {
  test_spy::gps_started = true;
  return true;
}

void GpsService::stop() { test_spy::gps_stopped = true; }

GpsData GpsService::get_latest_fix() const { return GpsData{}; }

void GpsService::set_posting_interval_ms(int interval_ms) {
  test_spy::gps_posting_interval_ms = interval_ms;
}

GpsData gps_read_once(GpsSensor & /*gps*/, int /*baud_rate*/, uint32_t /*timeout_ms*/) {
  return GpsData{};
}

// ============================================================================
// InputService stubs
// ============================================================================

InputService::InputService(CapTouchSensor &touch, const gpio::Hal &gpio,
                           RtosQueueHandle event_queue, const Config &config)
    : _touch(touch), _gpio(gpio), _event_queue(event_queue), _config(config) {}

InputService::~InputService() = default;

bool InputService::start() {
  test_spy::input_started = true;
  return true;
}

void InputService::stop() { test_spy::input_stopped = true; }

// ============================================================================
// StorageService stubs
// ============================================================================

StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}

bool StorageService::init() { return true; }

void StorageService::cache_measurement(const MeasuresAGo &m) {
  test_spy::cache_measurement_called = true;
  test_spy::last_cached_measurement = m;
}

uint16_t StorageService::read_cached_field(CacheField /*field*/, float * /*out*/,
                                           uint16_t /*max_count*/) const {
  return 0;
}

uint16_t StorageService::read_cache(MeasuresAGo * /*out*/, uint16_t /*max_count*/) const {
  return 0;
}

uint16_t StorageService::cached_count() const { return 0; }

void StorageService::backup_cache() const { test_spy::cache_backed_up = true; }

void StorageService::restore_cache() { test_spy::cache_restored = true; }

bool StorageService::start_route(uint32_t session_id) {
  test_spy::route_started = true;
  test_spy::route_session_id = session_id;
  return true;
}

bool StorageService::append_route_point(const RoutePoint &point) {
  test_spy::route_point_appended = true;
  test_spy::last_route_point = point;
  return true;
}

void StorageService::end_route() { test_spy::route_ended = true; }

bool StorageService::is_route_active() const {
  return test_spy::route_started && !test_spy::route_ended;
}

uint32_t StorageService::current_route_point_count() const { return 0; }

// ============================================================================
// PowerService stubs
// ============================================================================

PowerService::PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config)
    : _bms(bms), _gpio(gpio), _config(config) {}

PowerSnapshot PowerService::poll_bms() {
  test_spy::bms_polled = true;
  return test_spy::snapshot_to_return;
}

bool PowerService::reset_watchdog() {
  test_spy::watchdog_reset = true;
  return true;
}

void PowerService::shutdown() { test_spy::shutdown_called = true; }

void PowerService::save_state(const RtcAppState &state) {
  test_spy::state_saved = true;
  test_spy::last_saved_state = state;
}

RtcAppState PowerService::load_state() const { return test_spy::state_to_load; }

PowerService::SleepType PowerService::evaluate_sleep(const GoSettings & /*settings*/,
                                                     LockState /*lock_state*/) const {
  return test_spy::sleep_type_to_return;
}

WakeCause PowerService::enter_sleep(SleepType /*type*/, uint32_t /*sleep_duration_ms*/) {
  return test_spy::sleep_wake_cause_to_return;
}

WakeCause PowerService::get_wake_cause() { return WakeCause::PowerOn; }

bool PowerService::is_fast_path_wake(WakeCause /*cause*/, const RtcAppState & /*state*/) {
  return false;
}

// ============================================================================
// Free functions from go_power.h
// ============================================================================

RtcAppState load_rtc_app_state() { return test_spy::state_to_load; }

// ============================================================================
// Private stubs that are never called but must link
// ============================================================================

void PowerService::configure_wake_sources(uint32_t /*timer_ms*/) {}

bool StorageService::ensure_route_dir() const { return true; }

// SensorProducer private methods
void SensorProducer::task_entry(void * /*arg*/) {}
void SensorProducer::run() {}

// GpsService private methods
void GpsService::task_entry(void * /*arg*/) {}
void GpsService::run() {}
void GpsService::update_latest_fix(const GpsData & /*data*/) {}
void GpsService::post_fix_event() {}
void GpsService::sync_system_clock(const GpsTimestamp & /*ts*/) {}

// InputService private methods
void InputService::process_touch_interrupt() {}
void InputService::process_button_event(InputSource /*source*/, uint64_t /*timestamp_ms*/) {}
void InputService::check_pending_long_press() {}
uint32_t InputService::compute_queue_timeout_ms() const { return UINT32_MAX; }
void InputService::post_input_event(InputSource /*source*/, InputType /*type*/) {}
void InputService::cap_int_isr(void * /*arg*/) {}
void InputService::button_power_isr(void * /*arg*/) {}
void InputService::button_boot_isr(void * /*arg*/) {}
void InputService::task_entry(void * /*arg*/) {}
void InputService::run() {}
int InputService::pin_for_button_index(int /*idx*/) const { return -1; }
