/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#if defined(__has_include)
#if __has_include(<catch2/catch_test_macros.hpp>)

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "rtos.h"

namespace {

class TestRTOS : public RTOS {
public:
  void delay_ms_impl(uint32_t) override {}
  uint64_t get_time_ms_impl() override { return 0; }
};

class ScopedRtosInstance {
public:
  explicit ScopedRtosInstance(RTOS &rtos) { RTOS::set_instance(&rtos); }
  ~ScopedRtosInstance() { RTOS::set_instance(nullptr); }
};

} // namespace

TEST_CASE("RTOS queue_send reports successful admission", "[rtos][queue]") {
  TestRTOS rtos;
  ScopedRtosInstance instance(rtos);
  RtosQueueHandle queue = RTOS::queue_create(1, sizeof(uint32_t));
  REQUIRE(queue != nullptr);

  const uint32_t sent = 42;
  REQUIRE(RTOS::queue_send(queue, &sent));

  uint32_t received = 0;
  REQUIRE(RTOS::queue_receive(queue, &received, 0));
  REQUIRE(received == sent);
  RTOS::queue_delete(queue);
}

TEST_CASE("RTOS queue_send reports a full queue", "[rtos][queue]") {
  TestRTOS rtos;
  ScopedRtosInstance instance(rtos);
  RtosQueueHandle queue = RTOS::queue_create(1, sizeof(uint32_t));
  REQUIRE(queue != nullptr);

  const uint32_t first = 1;
  const uint32_t second = 2;
  REQUIRE(RTOS::queue_send(queue, &first));
  REQUIRE_FALSE(RTOS::queue_send(queue, &second));

  uint32_t received = 0;
  REQUIRE(RTOS::queue_receive(queue, &received, 0));
  REQUIRE(received == first);
  RTOS::queue_delete(queue);
}

TEST_CASE("RTOS queue_send rejects invalid arguments", "[rtos][queue]") {
  TestRTOS rtos;
  ScopedRtosInstance instance(rtos);
  const uint32_t item = 1;

  REQUIRE_FALSE(RTOS::queue_send(nullptr, &item));

  RtosQueueHandle queue = RTOS::queue_create(1, sizeof(item));
  REQUIRE(queue != nullptr);
  REQUIRE_FALSE(RTOS::queue_send(queue, nullptr));
  RTOS::queue_delete(queue);
}

TEST_CASE("RTOS queue_send fails without a host instance", "[rtos][queue]") {
  RTOS::set_instance(nullptr);
  uint8_t queue_sentinel = 0;
  const uint32_t item = 1;

  REQUIRE_FALSE(RTOS::queue_send(&queue_sentinel, &item));
}

TEST_CASE("RTOS queue_send remains compatible with ignored returns", "[rtos][queue]") {
  TestRTOS rtos;
  ScopedRtosInstance instance(rtos);
  RtosQueueHandle queue = RTOS::queue_create(1, sizeof(uint32_t));
  REQUIRE(queue != nullptr);

  const uint32_t sent = 42;
  RTOS::queue_send(queue, &sent);

  uint32_t received = 0;
  REQUIRE(RTOS::queue_receive(queue, &received, 0));
  REQUIRE(received == sent);
  RTOS::queue_delete(queue);
}

#endif // has Catch2
#endif // __has_include
