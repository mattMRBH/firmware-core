/**
 * AirGradient Go -- retained monotonic uptime host tests
 */

#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "go_uptime.h"
#include "rtos.h"

namespace {

constexpr uint64_t MILLISECONDS_PER_MINUTE = 60'000ULL;

class TestRtos final : public RTOS {
public:
  void delay_ms_impl(uint32_t) override {}
  uint64_t get_time_ms_impl() override { return 0; }
  uint64_t get_retained_time_ms_impl() override { return retained_time_ms; }

  uint64_t retained_time_ms = 0;
};

class Fixture {
public:
  Fixture() {
    RTOS::set_instance(&rtos);
    go_uptime_reset_retained_state_for_test();
  }

  ~Fixture() {
    go_uptime_reset_retained_state_for_test();
    RTOS::set_instance(nullptr);
  }

  TestRtos rtos;
};

} // namespace

TEST_CASE("Go uptime floors completed minutes at boundaries", "[go][uptime]") {
  Fixture fixture;
  go_uptime_init();

  fixture.rtos.retained_time_ms = MILLISECONDS_PER_MINUTE - 1;
  CHECK(go_uptime_minutes() == 0);

  fixture.rtos.retained_time_ms = MILLISECONDS_PER_MINUTE;
  CHECK(go_uptime_minutes() == 1);

  fixture.rtos.retained_time_ms = (2 * MILLISECONDS_PER_MINUTE) - 1;
  CHECK(go_uptime_minutes() == 1);

  fixture.rtos.retained_time_ms = 2 * MILLISECONDS_PER_MINUTE;
  CHECK(go_uptime_minutes() == 2);
}

TEST_CASE("Go uptime advances across a simulated deep-sleep wake", "[go][uptime]") {
  Fixture fixture;
  fixture.rtos.retained_time_ms = 10'000;
  go_uptime_init();

  fixture.rtos.retained_time_ms = 70'000;
  CHECK(go_uptime_minutes() == 1);

  // Deep sleep restarts the CPU, but both RTC data and the retained clock continue.
  fixture.rtos.retained_time_ms = 100'000;
  go_uptime_init();
  fixture.rtos.retained_time_ms = 130'000;
  CHECK(go_uptime_minutes() == 2);
}

TEST_CASE("Go uptime restarts after a simulated non-deep-sleep reset", "[go][uptime]") {
  Fixture fixture;
  go_uptime_init();
  fixture.rtos.retained_time_ms = 3 * MILLISECONDS_PER_MINUTE;
  REQUIRE(go_uptime_minutes() == 3);

  go_uptime_reset_retained_state_for_test();
  fixture.rtos.retained_time_ms = 200'000;
  go_uptime_init();
  CHECK(go_uptime_minutes() == 0);

  fixture.rtos.retained_time_ms = 200'000 + MILLISECONDS_PER_MINUTE;
  CHECK(go_uptime_minutes() == 1);
}

TEST_CASE("Go uptime recovers when the retained clock regresses", "[go][uptime]") {
  Fixture fixture;
  fixture.rtos.retained_time_ms = 100'000;
  go_uptime_init();

  fixture.rtos.retained_time_ms = 50'000;
  CHECK(go_uptime_minutes() == 0);

  fixture.rtos.retained_time_ms = 110'000;
  CHECK(go_uptime_minutes() == 1);
}

TEST_CASE("Go uptime saturates the uint32 minute value", "[go][uptime]") {
  Fixture fixture;
  go_uptime_init();

  const uint64_t overflow_minutes =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1ULL;
  fixture.rtos.retained_time_ms = overflow_minutes * MILLISECONDS_PER_MINUTE;
  CHECK(go_uptime_minutes() == std::numeric_limits<uint32_t>::max());

  fixture.rtos.retained_time_ms = std::numeric_limits<uint64_t>::max() - 1ULL;
  CHECK(go_uptime_minutes() == std::numeric_limits<uint32_t>::max());
}
