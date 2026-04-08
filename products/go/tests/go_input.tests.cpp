/**
 * AirGradient Go — InputService unit tests
 *
 * Tests the four logical processing functions in isolation:
 *   - process_touch_interrupt()  : channel mapping and noise filtering
 *   - process_button_event()     : debounce
 *   - check_pending_long_press() : long-press timing and GPIO level check
 *   - compute_queue_timeout_ms() : timeout calculation for queue receive
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "cap_touch_sensor.h"
#include "go_input.h"
#include "rtos.h"

// ============================================================================
// Mock: CapTouchSensor
// ============================================================================

class MockCapTouchSensor : public trompeloeil::mock_interface<CapTouchSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_MOCK0(clear_interrupt);
  IMPLEMENT_CONST_MOCK0(supports_noise);
  IMPLEMENT_CONST_MOCK0(supports_calibration);
  IMPLEMENT_MOCK1(calibrate);
};

// ============================================================================
// Mock: RTOS
// ============================================================================

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
};

// ============================================================================
// GPIO HAL stubs
//
// gpio::Hal is a plain function-pointer table, not a class.  Stubs are plain
// C++ functions; test_gpio_level[] simulates GPIO pin state (tests write
// directly: test_gpio_level[pin] = 0 or 1).
// ============================================================================

static int test_gpio_level[8] = {};

static bool gpio_configure(int, gpio::Mode, gpio::PullMode, gpio::InterruptType) { return true; }
static int gpio_get_level(int pin) { return test_gpio_level[pin]; }
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
// Test helpers
// ============================================================================

static constexpr int PIN_CAP_INT = 0;
static constexpr int PIN_BTN_POWER = 1;
static constexpr int PIN_BTN_BOOT = 2;

// GPIO levels matching BUTTON_PRESSED_LEVEL in go_input.cpp (active-low).
static constexpr int BTN_PRESSED = 0;
static constexpr int BTN_RELEASED = 1;

static InputService::Config make_config() {
  InputService::Config cfg{};
  cfg.pin_cap_int = PIN_CAP_INT;
  cfg.pin_button_power = PIN_BTN_POWER;
  cfg.pin_button_boot = PIN_BTN_BOOT;
  cfg.debounce_ms = 50;
  cfg.long_press_ms = 2000;
  return cfg;
}

// TestableInputService — subclass that:
//  - exposes the protected logic methods under their original names
//  - overrides post_input_event() to capture events in a vector instead of
//    sending them to a FreeRTOS queue (event_queue is passed as nullptr)
class TestableInputService : public InputService {
public:
  struct PostedEvent {
    InputSource source;
    InputType type;
  };

  std::vector<PostedEvent> events;

  TestableInputService(CapTouchSensor &touch, const InputService::Config &cfg)
      : InputService(touch, test_gpio_hal, nullptr, cfg) {}

  using InputService::check_pending_long_press;
  using InputService::check_touch_health;
  using InputService::compute_queue_timeout_ms;
  using InputService::process_button_event;
  using InputService::process_touch_interrupt;

protected:
  void post_input_event(InputSource source, InputType type) override {
    events.push_back({source, type});
  }
};

// ============================================================================
// TEST CASE 1 — Touch interrupt processing
// ============================================================================

TEST_CASE("Touch interrupt processing", "[InputService][touch]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  // Touch debounce calls RTOS::get_time_ms(); return a value well past the
  // debounce window so each test section's single call is always accepted.
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(10000);

  TestableInputService svc(mock_touch, make_config());

  SECTION("CH1 touched → TouchDown ShortPress") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchDown);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("CH2 touched → TouchUp ShortPress") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchUp);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("CH3 touched → TouchEnter ShortPress") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH3, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchEnter);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("Multiple channels touched → events in CH1 / CH2 / CH3 order") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::ALL, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 3);
    CHECK(svc.events[0].source == InputSource::TouchDown);
    CHECK(svc.events[1].source == InputSource::TouchUp);
    CHECK(svc.events[2].source == InputSource::TouchEnter);
  }

  SECTION("Noisy channel filtered out — no event posted") {
    // CH1 is both touched and noisy: valid = CH1 & ~CH1 = 0
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, TouchChannel::CH1})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }

  SECTION("Two channels touched, one noisy → only clean channel fires") {
    // CH1 and CH2 touched; CH1 noisy: valid = (CH1|CH2) & ~CH1 = CH2
    const uint8_t touched = static_cast<uint8_t>(TouchChannel::CH1 | TouchChannel::CH2);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{touched, TouchChannel::CH1})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchUp);
  }

  SECTION("All channels noisy → zero events posted") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::ALL, TouchChannel::ALL})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }

  SECTION("read() fails → no events; clear_interrupt still called") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_)).RETURN(false);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }

  SECTION("Noise present + supports_calibration true → calibrate called with noise mask") {
    const uint8_t noise_mask = TouchChannel::CH2;
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{0, noise_mask})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(true);
    REQUIRE_CALL(mock_touch, calibrate(noise_mask)).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty()); // touched == 0, nothing to post
  }

  SECTION("Noise present + supports_calibration false → calibrate not called") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{0, TouchChannel::CH1})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);
    // No calibrate expectation: trompeloeil fails the test if calibrate() is called.

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }
}

// ============================================================================
// TEST CASE 2 — Button debounce
// ============================================================================

TEST_CASE("Button debounce", "[InputService][button]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  // Reset GPIO state: buttons pressed (active-low) for press-down tests.
  test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
  test_gpio_level[PIN_BTN_BOOT] = BTN_PRESSED;

  TestableInputService svc(mock_touch, make_config());

  SECTION("First press accepted — long-press timer armed") {
    // process_button_event does not post an event immediately; it only arms
    // the long-press timer.  The event fires later in check_pending_long_press.
    svc.process_button_event(InputSource::ButtonPower, 1000);

    CHECK(svc.events.empty());
    // Verify timer is armed: at RTOS t=1800, elapsed=800ms, remaining=1200ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 1200);
  }

  SECTION("Press within debounce window rejected — timer stays at original time") {
    svc.process_button_event(InputSource::ButtonPower, 0);  // t=0, accepted
    svc.process_button_event(InputSource::ButtonPower, 30); // t=30ms < 50ms, rejected

    // Timer must still be armed at t=0.
    // At RTOS t=1800: elapsed=1800ms, remaining=200ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 200);
  }

  SECTION("Press after debounce window accepted — timer re-armed at new timestamp") {
    svc.process_button_event(InputSource::ButtonPower, 0);  // t=0, accepted
    svc.process_button_event(InputSource::ButtonPower, 60); // t=60ms > 50ms, accepted

    // Timer re-armed at t=60.
    // At RTOS t=1800: elapsed=1740ms, remaining=260ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 260);
  }

  SECTION("ButtonBoot debounce is independent of ButtonPower") {
    svc.process_button_event(InputSource::ButtonPower, 0);
    svc.process_button_event(InputSource::ButtonBoot, 10); // < 50ms from Power t=0,
                                                           // but Boot has its own counter

    // Both timers should be armed independently.
    // At RTOS t=1800: Power remaining=200ms, Boot remaining=210ms → min=200ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 200);
  }

  SECTION("Release before timeout → immediate ShortPress") {
    // Press-down: arm timer
    svc.process_button_event(InputSource::ButtonPower, 1000);
    CHECK(svc.events.empty());

    // Release: GPIO level goes high → immediate ShortPress, no waiting
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;
    svc.process_button_event(InputSource::ButtonPower, 1150);

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonPower);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("Release cancels pending long-press timer") {
    // Press-down
    svc.process_button_event(InputSource::ButtonPower, 500);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(600);
    CHECK(svc.compute_queue_timeout_ms() < make_config().touch_watchdog_ms); // timer pending

    // Release
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;
    svc.process_button_event(InputSource::ButtonPower, 600);

    // Timer should be disarmed: falls back to watchdog interval
    CHECK(svc.compute_queue_timeout_ms() == make_config().touch_watchdog_ms);
  }

  SECTION("Release bounce does not duplicate ShortPress") {
    // Press-down
    svc.process_button_event(InputSource::ButtonPower, 1000);

    // First release → ShortPress posted
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;
    svc.process_button_event(InputSource::ButtonPower, 1100);

    // Bounce: second release ISR fires — _pending_long_press already false
    svc.process_button_event(InputSource::ButtonPower, 1105);

    REQUIRE(svc.events.size() == 1); // only one ShortPress
  }
}

// ============================================================================
// TEST CASE 3 — Long-press detection
// ============================================================================

TEST_CASE("Long-press detection", "[InputService][button]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  TestableInputService svc(mock_touch, make_config());

  SECTION("Timer not yet expired → no event posted") {
    // Press-down: GPIO must be low when process_button_event is called.
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    // At t=1999: elapsed=1999ms < 2000ms threshold.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1999);
    svc.check_pending_long_press();

    CHECK(svc.events.empty());
  }

  SECTION("Timer expired, button still held → LongPress") {
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    svc.check_pending_long_press();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonPower);
    CHECK(svc.events[0].type == InputType::LongPress);
  }

  SECTION("Timer expired, button already released → ShortPress (safety net)") {
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    // Button released before timer expired, but no release ISR fired
    // (edge case / missed interrupt). check_pending_long_press handles it.
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    svc.check_pending_long_press();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonPower);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("After firing, pending flag cleared — second check posts nothing") {
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    svc.check_pending_long_press(); // fires LongPress, clears pending flag
    svc.events.clear();
    svc.check_pending_long_press(); // timer is disarmed — must post nothing

    CHECK(svc.events.empty());
  }

  SECTION("ButtonBoot long-press is independent of ButtonPower") {
    test_gpio_level[PIN_BTN_BOOT] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonBoot, 0);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2500);
    svc.check_pending_long_press();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonBoot);
    CHECK(svc.events[0].type == InputType::LongPress);
  }
}

// ============================================================================
// TEST CASE 4 — Wake-press suppression
// ============================================================================
//
// When Config::suppress_button_wake = true, the first ButtonPower press-down
// is silently discarded so the wake press cannot generate a spurious ShortPress
// that would re-lock the device immediately after a button-wake boot.

TEST_CASE("Wake-press suppression", "[InputService][suppress]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);

  // Buttons pressed (active-low) for press-down tests.
  test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;

  SECTION("suppress=true: first ButtonPower press-down is discarded") {
    auto cfg = make_config();
    cfg.suppress_button_wake = true;
    TestableInputService svc(mock_touch, cfg);

    svc.process_button_event(InputSource::ButtonPower, 1000);

    // No event, no long-press timer armed.
    CHECK(svc.events.empty());
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1001);
    CHECK(svc.compute_queue_timeout_ms() == cfg.touch_watchdog_ms); // no timer pending
  }

  SECTION("suppress=true: suppression is one-shot; second press arms timer normally") {
    auto cfg = make_config();
    cfg.suppress_button_wake = true;
    TestableInputService svc(mock_touch, cfg);

    // First press: suppressed.
    svc.process_button_event(InputSource::ButtonPower, 0);
    CHECK(svc.events.empty());

    // Second press (past debounce): accepted, timer armed.
    svc.process_button_event(InputSource::ButtonPower, 1000);
    CHECK(svc.events.empty()); // still no event — waits for release or timeout
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1001);
    CHECK(svc.compute_queue_timeout_ms() < cfg.touch_watchdog_ms); // timer is now pending
  }

  SECTION("suppress=false (default): first ButtonPower press arms timer normally") {
    TestableInputService svc(mock_touch, make_config()); // suppress_button_wake defaults to false

    svc.process_button_event(InputSource::ButtonPower, 1000);

    CHECK(svc.events.empty()); // no immediate event — awaiting release/timeout
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1001);
    CHECK(svc.compute_queue_timeout_ms() < make_config().touch_watchdog_ms); // timer is pending
  }

  SECTION("suppress=true: touch events are unaffected") {
    auto cfg = make_config();
    cfg.suppress_button_wake = true;
    TestableInputService svc(mock_touch, cfg);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(10000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH3, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchEnter);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }
}

// ============================================================================
// TEST CASE 5 — Queue timeout computation (was TEST CASE 4)
// ============================================================================

TEST_CASE("Queue timeout computation", "[InputService][timeout]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  // Buttons must read as pressed to arm timers via process_button_event.
  test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
  test_gpio_level[PIN_BTN_BOOT] = BTN_PRESSED;

  TestableInputService svc(mock_touch, make_config());

  SECTION("No pending presses → touch_watchdog_ms (periodic health check)") {
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(5000);
    CHECK(svc.compute_queue_timeout_ms() == make_config().touch_watchdog_ms);
  }

  SECTION("One pending press with time remaining → remaining ms") {
    // Armed at t=500; queried at t=1000: elapsed=500ms, remaining=1500ms.
    svc.process_button_event(InputSource::ButtonPower, 500);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);
    CHECK(svc.compute_queue_timeout_ms() == 1500);
  }

  SECTION("Pending press already past threshold → 0 (wake immediately)") {
    svc.process_button_event(InputSource::ButtonPower, 0);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(3000); // 3000 > 2000ms
    CHECK(svc.compute_queue_timeout_ms() == 0);
  }

  SECTION("Two pending presses → minimum remaining time") {
    // Power armed at t=500; at t=1600 elapsed=1100, remaining=900ms.
    // Boot  armed at t=1000; at t=1600 elapsed=600,  remaining=1400ms.
    // Expected: min(900, 1400) = 900ms.
    svc.process_button_event(InputSource::ButtonPower, 500);
    svc.process_button_event(InputSource::ButtonBoot, 1000);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1600);
    CHECK(svc.compute_queue_timeout_ms() == 900);
  }

  SECTION("Timeout capped at touch_watchdog_ms, not UINT32_MAX") {
    // With a long-press remaining time larger than the watchdog interval,
    // the watchdog interval should win.
    auto cfg = make_config();
    cfg.touch_watchdog_ms = 500;
    cfg.long_press_ms = 5000;
    TestableInputService svc2(mock_touch, cfg);

    // Press at t=0, query at t=1 → long-press remaining=4999ms > 500ms watchdog.
    svc2.process_button_event(InputSource::ButtonPower, 0);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1);
    CHECK(svc2.compute_queue_timeout_ms() == 500);
  }
}

// ============================================================================
// TEST CASE 6 — Touch health watchdog
// ============================================================================

TEST_CASE("Touch health watchdog", "[InputService][touch][watchdog]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(10000);

  TestableInputService svc(mock_touch, make_config());

  SECTION("INT deasserted (HIGH) → no recovery attempted") {
    test_gpio_level[PIN_CAP_INT] = 1; // INT HIGH — healthy

    // No mock expectations on clear_interrupt or init — trompeloeil will fail
    // the test if they are called unexpectedly.
    svc.check_touch_health();

    CHECK(svc.events.empty());
  }

  SECTION("INT stuck LOW → clear_interrupt recovers") {
    test_gpio_level[PIN_CAP_INT] = 0; // INT LOW — stuck

    REQUIRE_CALL(mock_touch, clear_interrupt())
        .LR_SIDE_EFFECT(test_gpio_level[PIN_CAP_INT] = 1) // simulate recovery
        .RETURN(true);

    svc.check_touch_health();

    CHECK(svc.events.empty()); // health check doesn't post events
  }

  SECTION("INT stuck LOW + clear_interrupt fails to recover → escalates to init") {
    test_gpio_level[PIN_CAP_INT] = 0; // INT LOW — stuck

    // clear_interrupt succeeds but INT stays LOW
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    // Escalation: full re-init
    REQUIRE_CALL(mock_touch, init())
        .LR_SIDE_EFFECT(test_gpio_level[PIN_CAP_INT] = 1) // simulate recovery
        .RETURN(true);

    svc.check_touch_health();

    CHECK(svc.events.empty());
  }

  SECTION("INT stuck LOW + both clear_interrupt and init fail → no crash") {
    test_gpio_level[PIN_CAP_INT] = 0; // INT LOW — stuck

    // clear_interrupt fails
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(false);
    // init also fails
    REQUIRE_CALL(mock_touch, init()).RETURN(false);

    svc.check_touch_health(); // must not crash

    CHECK(svc.events.empty());
  }
}
