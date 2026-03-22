/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#if defined(__has_include)
#if __has_include(<catch2/catch_test_macros.hpp>) && __has_include(<trompeloeil.hpp>) && __has_include(<trompeloeil/mock.hpp>)

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "hal/payload_cache_storage.h"
#include "services/payload_cache.h"

class MockPayloadCacheStorage : public trompeloeil::mock_interface<PayloadCacheStorage> {
public:
  IMPLEMENT_MOCK1(load);
  IMPLEMENT_MOCK1(save);
  IMPLEMENT_MOCK0(clear);
};

namespace {

PayloadCacheType make_payload(int seed) {
#if defined(CONFIG_PAYLOAD_CACHE_TYPE_BASIC)
  return {{seed + 0.1f, seed + 0.2f},
          {seed + 2.1f, seed + 2.2f, seed + 2.3f, seed + 2.4f, seed + 2.5f, seed + 2.6f,
           seed + 2.7f, seed + 2.8f, seed + 2.9f, seed + 3.0f, seed + 3.1f, seed + 3.2f},
          {seed + 100},
          {seed + 200, seed + 201, seed + 202, seed + 203}};
#else
  return {{seed + 0.1f, seed + 0.2f},
          {seed + 1.1f, seed + 1.2f},
          {seed + 2.1f, seed + 2.2f, seed + 2.3f, seed + 2.4f, seed + 2.5f, seed + 2.6f,
           seed + 2.7f, seed + 2.8f, seed + 2.9f, seed + 3.0f, seed + 3.1f, seed + 3.2f},
          {seed + 4.1f, seed + 4.2f, seed + 4.3f, seed + 4.4f, seed + 4.5f, seed + 4.6f,
           seed + 4.7f, seed + 4.8f, seed + 4.9f, seed + 5.0f, seed + 5.1f, seed + 5.2f},
          {seed + 100},
          {seed + 200, seed + 201, seed + 202, seed + 203},
          {seed + 6.1f, seed + 6.2f},
          {seed + 7.1f, seed + 7.2f, seed + 7.3f, seed + 7.4f, seed + 7.5f}};
#endif
}

void require_payload_equal(const PayloadCacheType &actual, const PayloadCacheType &expected) {
  REQUIRE(actual.temp_hum_a.temperature == expected.temp_hum_a.temperature);
  REQUIRE(actual.temp_hum_a.humidity == expected.temp_hum_a.humidity);

#if defined(CONFIG_PAYLOAD_CACHE_TYPE_FULL)
  REQUIRE(actual.temp_hum_b.temperature == expected.temp_hum_b.temperature);
  REQUIRE(actual.temp_hum_b.humidity == expected.temp_hum_b.humidity);
#endif

  REQUIRE(actual.pm_a.pm_01 == expected.pm_a.pm_01);
  REQUIRE(actual.pm_a.pm_25 == expected.pm_a.pm_25);
  REQUIRE(actual.pm_a.pm_10 == expected.pm_a.pm_10);
  REQUIRE(actual.pm_a.pm_01_sp == expected.pm_a.pm_01_sp);
  REQUIRE(actual.pm_a.pm_25_sp == expected.pm_a.pm_25_sp);
  REQUIRE(actual.pm_a.pm_10_sp == expected.pm_a.pm_10_sp);
  REQUIRE(actual.pm_a.pm_03_pc == expected.pm_a.pm_03_pc);
  REQUIRE(actual.pm_a.pm_05_pc == expected.pm_a.pm_05_pc);
  REQUIRE(actual.pm_a.pm_01_pc == expected.pm_a.pm_01_pc);
  REQUIRE(actual.pm_a.pm_25_pc == expected.pm_a.pm_25_pc);
  REQUIRE(actual.pm_a.pm_5_pc == expected.pm_a.pm_5_pc);
  REQUIRE(actual.pm_a.pm_10_pc == expected.pm_a.pm_10_pc);

  REQUIRE(actual.co2.co2 == expected.co2.co2);

  REQUIRE(actual.tvoc_nox.tvoc_index == expected.tvoc_nox.tvoc_index);
  REQUIRE(actual.tvoc_nox.tvoc_raw == expected.tvoc_nox.tvoc_raw);
  REQUIRE(actual.tvoc_nox.nox_index == expected.tvoc_nox.nox_index);
  REQUIRE(actual.tvoc_nox.nox_raw == expected.tvoc_nox.nox_raw);

#if defined(CONFIG_PAYLOAD_CACHE_TYPE_FULL)
  REQUIRE(actual.pm_b.pm_01 == expected.pm_b.pm_01);
  REQUIRE(actual.pm_b.pm_25 == expected.pm_b.pm_25);
  REQUIRE(actual.pm_b.pm_10 == expected.pm_b.pm_10);
  REQUIRE(actual.pm_b.pm_01_sp == expected.pm_b.pm_01_sp);
  REQUIRE(actual.pm_b.pm_25_sp == expected.pm_b.pm_25_sp);
  REQUIRE(actual.pm_b.pm_10_sp == expected.pm_b.pm_10_sp);
  REQUIRE(actual.pm_b.pm_03_pc == expected.pm_b.pm_03_pc);
  REQUIRE(actual.pm_b.pm_05_pc == expected.pm_b.pm_05_pc);
  REQUIRE(actual.pm_b.pm_01_pc == expected.pm_b.pm_01_pc);
  REQUIRE(actual.pm_b.pm_25_pc == expected.pm_b.pm_25_pc);
  REQUIRE(actual.pm_b.pm_5_pc == expected.pm_b.pm_5_pc);
  REQUIRE(actual.pm_b.pm_10_pc == expected.pm_b.pm_10_pc);

  REQUIRE(actual.power.battery_voltage == expected.power.battery_voltage);
  REQUIRE(actual.power.charging_voltage == expected.power.charging_voltage);

  REQUIRE(actual.electrode.o3_we == expected.electrode.o3_we);
  REQUIRE(actual.electrode.o3_ae == expected.electrode.o3_ae);
  REQUIRE(actual.electrode.no2_we == expected.electrode.no2_we);
  REQUIRE(actual.electrode.no2_ae == expected.electrode.no2_ae);
  REQUIRE(actual.electrode.afe_temp == expected.electrode.afe_temp);
#endif
}

} // namespace

TEST_CASE("Payload cache", "[PayloadCache]") {
  MockPayloadCacheStorage mock_storage;
  PayloadCache cache(mock_storage, 4);

  const PayloadCacheType payload_a = make_payload(10);
  const PayloadCacheType payload_b = make_payload(20);
  const PayloadCacheType payload_c = make_payload(30);
  const PayloadCacheType payload_d = make_payload(40);

  SECTION("restore loads saved queue state") {
    PayloadCacheStorageData stored = {};
    stored.head = 0;
    stored.tail = 2;
    stored.payloads[0] = payload_a;
    stored.payloads[1] = payload_b;

    REQUIRE_CALL(mock_storage, load(trompeloeil::_)).LR_SIDE_EFFECT(_1 = stored).RETURN(true);

    cache.restore();

    REQUIRE(cache.get_size() == 2);

    PayloadCacheType actual = {};
    REQUIRE(cache.peek_at_index(0, actual));
    require_payload_equal(actual, payload_a);
    REQUIRE(cache.peek_at_index(1, actual));
    require_payload_equal(actual, payload_b);
  }

  SECTION("restore resets queue when storage load fails") {
    REQUIRE_CALL(mock_storage, load(trompeloeil::_)).RETURN(false);

    cache.restore();

    REQUIRE(cache.get_size() == 0);
    PayloadCacheType actual = {};
    REQUIRE_FALSE(cache.peek_at_index(0, actual));
  }

  SECTION("restore resets queue when stored indices are out of range") {
    PayloadCacheStorageData stored = {};
    stored.head = 4;
    stored.tail = 1;

    REQUIRE_CALL(mock_storage, load(trompeloeil::_)).LR_SIDE_EFFECT(_1 = stored).RETURN(true);

    cache.restore();

    REQUIRE(cache.get_size() == 0);
    PayloadCacheType actual = {};
    REQUIRE_FALSE(cache.peek_at_index(0, actual));
  }

  SECTION("push stores payload snapshot") {
    REQUIRE_CALL(mock_storage, save(trompeloeil::_))
        .WITH(_1.head == 0 && _1.tail == 1 && _1.payloads[0].co2.co2 == payload_a.co2.co2)
        .RETURN(true);

    cache.push(payload_a);

    REQUIRE(cache.get_size() == 1);
    PayloadCacheType actual = {};
    REQUIRE(cache.peek_at_index(0, actual));
    require_payload_equal(actual, payload_a);
  }

  SECTION("pop returns false when queue is empty") {
    PayloadCacheType actual = {};
    REQUIRE_FALSE(cache.pop(actual));
    REQUIRE(cache.get_size() == 0);
  }

  SECTION("pop returns oldest payload and saves advanced head") {
    PayloadCacheStorageData stored = {};
    stored.head = 0;
    stored.tail = 2;
    stored.payloads[0] = payload_a;
    stored.payloads[1] = payload_b;

    REQUIRE_CALL(mock_storage, load(trompeloeil::_)).LR_SIDE_EFFECT(_1 = stored).RETURN(true);
    REQUIRE_CALL(mock_storage, save(trompeloeil::_))
        .WITH(_1.head == 1 && _1.tail == 2)
        .RETURN(true);

    cache.restore();

    PayloadCacheType actual = {};
    REQUIRE(cache.pop(actual));
    require_payload_equal(actual, payload_a);
    REQUIRE(cache.get_size() == 1);
    REQUIRE(cache.peek_at_index(0, actual));
    require_payload_equal(actual, payload_b);
  }

  SECTION("push overwrites oldest payload when queue becomes full") {
    ALLOW_CALL(mock_storage, save(trompeloeil::_)).RETURN(true);

    cache.push(payload_a);
    cache.push(payload_b);
    cache.push(payload_c);
    cache.push(payload_d);

    REQUIRE(cache.get_size() == 3);

    PayloadCacheType actual = {};
    REQUIRE(cache.peek_at_index(0, actual));
    require_payload_equal(actual, payload_b);
    REQUIRE(cache.peek_at_index(1, actual));
    require_payload_equal(actual, payload_c);
    REQUIRE(cache.peek_at_index(2, actual));
    require_payload_equal(actual, payload_d);
  }

  SECTION("peek_at_index returns false when out of range") {
    ALLOW_CALL(mock_storage, save(trompeloeil::_)).RETURN(true);

    cache.push(payload_a);

    PayloadCacheType actual = {};
    REQUIRE_FALSE(cache.peek_at_index(1, actual));
  }

  SECTION("clean resets queue and clears storage") {
    PayloadCacheStorageData stored = {};
    stored.head = 0;
    stored.tail = 2;
    stored.payloads[0] = payload_a;
    stored.payloads[1] = payload_b;

    REQUIRE_CALL(mock_storage, load(trompeloeil::_)).LR_SIDE_EFFECT(_1 = stored).RETURN(true);
    REQUIRE_CALL(mock_storage, clear()).RETURN(true);

    cache.restore();
    cache.clean();

    REQUIRE(cache.get_size() == 0);
    PayloadCacheType actual = {};
    REQUIRE_FALSE(cache.peek_at_index(0, actual));
  }

  SECTION("backup saves current queue snapshot") {
    PayloadCacheStorageData stored = {};
    stored.head = 0;
    stored.tail = 2;
    stored.payloads[0] = payload_a;
    stored.payloads[1] = payload_b;

    REQUIRE_CALL(mock_storage, load(trompeloeil::_)).LR_SIDE_EFFECT(_1 = stored).RETURN(true);
    REQUIRE_CALL(mock_storage, save(trompeloeil::_))
        .WITH(_1.head == 0 && _1.tail == 2 && _1.payloads[0].co2.co2 == payload_a.co2.co2 &&
              _1.payloads[1].co2.co2 == payload_b.co2.co2)
        .RETURN(true);

    cache.restore();
    cache.backup();
  }
}

#else

int payload_cache_tests_require_catch2_and_trompeloeil = 0;

#endif
#endif
