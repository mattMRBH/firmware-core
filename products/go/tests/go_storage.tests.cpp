/**
 * AirGradient Go — StorageService unit tests
 *
 * Covers two tiers independently:
 *
 * Cache tier  — read_cached_field() field extraction: valid values, invalid
 *               sentinels, oldest-first ordering, max_count cap.
 *
 * Route tier  — start/end lifecycle, file naming, append, binary round-trip,
 *               and sleep-resume (point count derived from file size).
 *
 * Route tests use a real POSIX temp directory so fopen/fwrite/stat/mkdir run
 * natively on the host without any ESP-IDF dependency.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "go_storage.h"
#include "hal/payload_cache_storage.h"
#include "nand_storage.h"
#include "services/payload_cache.h"

// ============================================================================
// MockNandStorage — for testing init() failure paths
// ============================================================================

class MockNandStorage : public trompeloeil::mock_interface<NandStorage> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK0(deinit);
  IMPLEMENT_CONST_MOCK0(is_mounted);
  IMPLEMENT_CONST_MOCK0(mount_path);
};

// ============================================================================
// FakeNandStorage — simple concrete stub for route-tier tests.
//
// Always reports mounted and returns the path supplied at construction.
// Avoids trompeloeil complexity for tests that only care about file I/O
// behaviour, not about how many times the NandStorage methods are called.
// ============================================================================

class FakeNandStorage : public NandStorage {
public:
  explicit FakeNandStorage(const char *path) : _path(path) {}

  bool init() override { return true; }
  void deinit() override {}
  bool is_mounted() const override { return true; }
  const char *mount_path() const override { return _path; }

private:
  const char *_path;
};

// ============================================================================
// StubPayloadCacheStorage
//
// Silent in-memory stub — load() always signals "no saved state" so the cache
// starts empty; save() silently succeeds.  Avoids per-test mock wiring noise
// for tests that only care about read_cached_field() behaviour.
// ============================================================================

class StubPayloadCacheStorage : public PayloadCacheStorage {
public:
  bool load(PayloadCacheStorageData &) override { return false; }
  bool save(const PayloadCacheStorageData &) override { return true; }
  bool clear() override { return true; }
};

// ============================================================================
// TempDir
//
// RAII helper that creates a unique temp directory via mkdtemp() and removes
// it (recursively) in the destructor.
// ============================================================================

struct TempDir {
  char path[256];

  TempDir() {
    std::snprintf(path, sizeof(path), "/tmp/go_storage_test_XXXXXX");
    REQUIRE(mkdtemp(path) != nullptr);
  }

  ~TempDir() { std::filesystem::remove_all(path); }

  // Returns the full path to a file under this directory.
  std::string operator/(const char *rel) const { return std::string(path) + "/" + rel; }
};

// ============================================================================
// Helpers
// ============================================================================

// Build a MeasuresBasic with distinct, in-range values seeded from an integer.
static MeasuresBasic make_valid_entry(int seed) {
  MeasuresBasic m{};
  m.temp_hum_a.temperature = static_cast<float>(seed) + 0.1f;   // valid: -40..125
  m.temp_hum_a.humidity = static_cast<float>(seed % 90) + 5.0f; // valid: 0..100
  m.pm_a.pm_01 = static_cast<float>(seed) + 1.0f;               // valid: >= 0
  m.pm_a.pm_25 = static_cast<float>(seed) + 2.0f;
  m.pm_a.pm_10 = static_cast<float>(seed) + 3.0f;
  m.co2.co2 = seed + 400;            // valid: 0..10000
  m.tvoc_nox.tvoc_index = seed + 10; // valid: >= 0
  m.tvoc_nox.nox_index = seed + 20;
  // fill remaining PMData sub-fields to valid (>= 0)
  m.pm_a.pm_01_sp = m.pm_a.pm_25_sp = m.pm_a.pm_10_sp = 0.0f;
  m.pm_a.pm_03_pc = m.pm_a.pm_05_pc = m.pm_a.pm_01_pc = 0.0f;
  m.pm_a.pm_25_pc = m.pm_a.pm_5_pc = m.pm_a.pm_10_pc = 0.0f;
  m.tvoc_nox.tvoc_raw = seed + 50;
  m.tvoc_nox.nox_raw = seed + 60;
  return m;
}

// Build an entry where every field carries an invalid sentinel value.
static MeasuresBasic make_invalid_entry() {
  MeasuresBasic m{};
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;          // -1000
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;                // -1
  m.pm_a.pm_01 = m.pm_a.pm_25 = m.pm_a.pm_10 = MeasuresInvalid::PM; // -1
  m.pm_a.pm_01_sp = m.pm_a.pm_25_sp = m.pm_a.pm_10_sp = MeasuresInvalid::PM;
  m.pm_a.pm_03_pc = m.pm_a.pm_05_pc = m.pm_a.pm_01_pc = MeasuresInvalid::PM;
  m.pm_a.pm_25_pc = m.pm_a.pm_5_pc = m.pm_a.pm_10_pc = MeasuresInvalid::PM;
  m.co2.co2 = MeasuresInvalid::CO2;              // -1
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC; // -1
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX; // -1
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  return m;
}

// Build a minimal RoutePoint with identifiable values for binary verification.
static RoutePoint make_route_point(int seed) {
  RoutePoint p{};
  p.timestamp = static_cast<time_t>(1000 + seed);
  p.gps.position.latitude = static_cast<double>(seed);
  p.gps.position.longitude = static_cast<double>(seed) + 0.5;
  p.sensors = make_valid_entry(seed);
  return p;
}

// ============================================================================
// TEST CASE 1 — Cache: read_cached_field field extraction
// ============================================================================

TEST_CASE("Cache: read_cached_field field extraction", "[StorageService][cache]") {
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  MockNandStorage mock_nand;
  StorageService svc(cache, mock_nand);

  static constexpr uint16_t BUF = 16;
  float out[BUF] = {};

  SECTION("empty cache returns 0") { CHECK(svc.read_cached_field(CacheField::CO2, out, BUF) == 0); }

  SECTION("Temperature: valid readings oldest-first") {
    svc.cache_measurement(make_valid_entry(10)); // temp = 10.1
    svc.cache_measurement(make_valid_entry(20)); // temp = 20.1
    svc.cache_measurement(make_valid_entry(30)); // temp = 30.1

    const uint16_t n = svc.read_cached_field(CacheField::Temperature, out, BUF);

    REQUIRE(n == 3);
    CHECK(out[0] == Catch::Approx(10.1f));
    CHECK(out[1] == Catch::Approx(20.1f));
    CHECK(out[2] == Catch::Approx(30.1f));
  }

  SECTION("Temperature: invalid entry writes MeasuresInvalid::TEMPERATURE") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::Temperature, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(MeasuresInvalid::TEMPERATURE));
  }

  SECTION("Humidity: valid entry") {
    svc.cache_measurement(make_valid_entry(5)); // humidity = (5 % 90) + 5 = 10.0

    const uint16_t n = svc.read_cached_field(CacheField::Humidity, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(10.0f));
  }

  SECTION("Humidity: invalid entry writes MeasuresInvalid::HUMIDITY") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::Humidity, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(MeasuresInvalid::HUMIDITY));
  }

  SECTION("PM01: valid entry") {
    svc.cache_measurement(make_valid_entry(7)); // pm_01 = 8.0

    const uint16_t n = svc.read_cached_field(CacheField::PM01, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(8.0f));
  }

  SECTION("PM25: valid entry") {
    svc.cache_measurement(make_valid_entry(7)); // pm_25 = 9.0

    const uint16_t n = svc.read_cached_field(CacheField::PM25, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(9.0f));
  }

  SECTION("PM10: valid entry") {
    svc.cache_measurement(make_valid_entry(7)); // pm_10 = 10.0

    const uint16_t n = svc.read_cached_field(CacheField::PM10, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(10.0f));
  }

  SECTION("PM01: invalid entry writes MeasuresInvalid::PM") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::PM01, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(MeasuresInvalid::PM));
  }

  SECTION("CO2: valid entry cast to float") {
    svc.cache_measurement(make_valid_entry(10)); // co2 = 410

    const uint16_t n = svc.read_cached_field(CacheField::CO2, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(410.0f));
  }

  SECTION("CO2: invalid entry writes float(MeasuresInvalid::CO2)") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::CO2, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(static_cast<float>(MeasuresInvalid::CO2)));
  }

  SECTION("TvocIndex: valid entry cast to float") {
    svc.cache_measurement(make_valid_entry(10)); // tvoc_index = 20

    const uint16_t n = svc.read_cached_field(CacheField::TvocIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(20.0f));
  }

  SECTION("TvocIndex: invalid entry writes float(MeasuresInvalid::TVOC)") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::TvocIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(static_cast<float>(MeasuresInvalid::TVOC)));
  }

  SECTION("NoxIndex: valid entry cast to float") {
    svc.cache_measurement(make_valid_entry(10)); // nox_index = 30

    const uint16_t n = svc.read_cached_field(CacheField::NoxIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(30.0f));
  }

  SECTION("NoxIndex: invalid entry writes float(MeasuresInvalid::NOX)") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::NoxIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(static_cast<float>(MeasuresInvalid::NOX)));
  }

  SECTION("mixed valid and invalid entries: each slot independently correct") {
    svc.cache_measurement(make_valid_entry(5));  // temp = 5.1
    svc.cache_measurement(make_invalid_entry()); // temp = MeasuresInvalid::TEMPERATURE
    svc.cache_measurement(make_valid_entry(15)); // temp = 15.1

    const uint16_t n = svc.read_cached_field(CacheField::Temperature, out, BUF);

    REQUIRE(n == 3);
    CHECK(out[0] == Catch::Approx(5.1f));
    CHECK(out[1] == Catch::Approx(MeasuresInvalid::TEMPERATURE));
    CHECK(out[2] == Catch::Approx(15.1f));
  }

  SECTION("max_count caps output to requested size") {
    for (int i = 0; i < 5; ++i) {
      svc.cache_measurement(make_valid_entry(i * 10));
    }

    const uint16_t n = svc.read_cached_field(CacheField::CO2, out, 3);

    REQUIRE(n == 3);
    // Only first 3 entries written — verify seed 0, 10, 20
    CHECK(out[0] == Catch::Approx(400.0f));
    CHECK(out[1] == Catch::Approx(410.0f));
    CHECK(out[2] == Catch::Approx(420.0f));
  }
}

// ============================================================================
// TEST CASE 2 — Route: init
// ============================================================================

TEST_CASE("Route: init", "[StorageService][route]") {
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  MockNandStorage mock_nand;
  StorageService svc(cache, mock_nand);

  SECTION("init succeeds when NAND mounts successfully") {
    REQUIRE_CALL(mock_nand, init()).RETURN(true);
    ALLOW_CALL(mock_nand, mount_path()).RETURN("/tmp");

    CHECK(svc.init() == true);
  }

  SECTION("init fails when NAND mount fails") {
    REQUIRE_CALL(mock_nand, init()).RETURN(false);

    CHECK(svc.init() == false);
  }
}

// ============================================================================
// TEST CASE 3 — Route: start/end lifecycle
// ============================================================================

TEST_CASE("Route: start/end lifecycle", "[StorageService][route]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("start_route creates a new route file; is_route_active becomes true") {
    CHECK_FALSE(svc.is_route_active());

    REQUIRE(svc.start_route(12345));

    CHECK(svc.is_route_active());
    CHECK(std::filesystem::exists(tmp / "routes/route_12345.bin"));

    svc.end_route();
  }

  SECTION("route file uses 5-digit zero-padded session ID in name") {
    REQUIRE(svc.start_route(10001));

    CHECK(std::filesystem::exists(tmp / "routes/route_10001.bin"));

    svc.end_route();
  }

  SECTION("start_route returns false when NAND is not mounted") {
    // Use a separate StorageService backed by a not-mounted NandStorage stub.
    MockNandStorage unmounted_nand;
    REQUIRE_CALL(unmounted_nand, is_mounted()).RETURN(false);
    StubPayloadCacheStorage stub2;
    PayloadCache cache2(stub2, 16);
    StorageService svc2(cache2, unmounted_nand);

    CHECK_FALSE(svc2.start_route(12345));
    CHECK_FALSE(svc2.is_route_active());
  }

  SECTION("start_route returns false when route is already active") {
    REQUIRE(svc.start_route(12345));
    CHECK_FALSE(svc.start_route(12345));
    CHECK(svc.is_route_active()); // still active from the first call

    svc.end_route();
  }

  SECTION("end_route closes file: is_route_active false, counts reset") {
    REQUIRE(svc.start_route(12345));
    svc.end_route();

    CHECK_FALSE(svc.is_route_active());
    CHECK(svc.current_route_point_count() == 0);
  }

  SECTION("end_route with no active route is a no-op") {
    CHECK_FALSE(svc.is_route_active());
    svc.end_route(); // must not crash
    CHECK_FALSE(svc.is_route_active());
  }
}

// ============================================================================
// TEST CASE 4 — Route: append_route_point
// ============================================================================

TEST_CASE("Route: append_route_point", "[StorageService][route]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("append returns false when no route is active") {
    CHECK_FALSE(svc.append_route_point(make_route_point(0)));
  }

  SECTION("single append increments point count to 1") {
    REQUIRE(svc.start_route(11111));

    CHECK(svc.append_route_point(make_route_point(1)));
    CHECK(svc.current_route_point_count() == 1);

    svc.end_route();
  }

  SECTION("multiple appends accumulate the count correctly") {
    REQUIRE(svc.start_route(22222));

    for (int i = 0; i < 5; ++i) {
      REQUIRE(svc.append_route_point(make_route_point(i)));
      CHECK(svc.current_route_point_count() == static_cast<uint32_t>(i + 1));
    }

    svc.end_route();
  }

  SECTION("binary round-trip: written RoutePoints match field-by-field when read back") {
    constexpr uint32_t SESSION_ID = 33333;
    const RoutePoint p0 = make_route_point(7);
    const RoutePoint p1 = make_route_point(42);

    REQUIRE(svc.start_route(SESSION_ID));
    REQUIRE(svc.append_route_point(p0));
    REQUIRE(svc.append_route_point(p1));
    svc.end_route();

    // Reopen the file and read back the raw structs.
    const std::string path = tmp / "routes/route_33333.bin";
    FILE *f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);

    RoutePoint read0{}, read1{};
    REQUIRE(fread(&read0, sizeof(RoutePoint), 1, f) == 1);
    REQUIRE(fread(&read1, sizeof(RoutePoint), 1, f) == 1);
    fclose(f);

    CHECK(read0.timestamp == p0.timestamp);
    CHECK(read0.gps.position.latitude == Catch::Approx(p0.gps.position.latitude));
    CHECK(read0.gps.position.longitude == Catch::Approx(p0.gps.position.longitude));
    CHECK(read0.sensors.co2.co2 == p0.sensors.co2.co2);

    CHECK(read1.timestamp == p1.timestamp);
    CHECK(read1.gps.position.latitude == Catch::Approx(p1.gps.position.latitude));
    CHECK(read1.gps.position.longitude == Catch::Approx(p1.gps.position.longitude));
    CHECK(read1.sensors.co2.co2 == p1.sensors.co2.co2);
  }
}

// ============================================================================
// TEST CASE 5 — Route: sleep resume
// ============================================================================

TEST_CASE("Route: sleep resume", "[StorageService][route]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("start_route with existing file opens in append mode; point count restored") {
    // First session: write 3 points.
    REQUIRE(svc.start_route(55555));
    for (int i = 0; i < 3; ++i) {
      REQUIRE(svc.append_route_point(make_route_point(i)));
    }
    svc.end_route();

    // Simulate wake: start same session again.
    REQUIRE(svc.start_route(55555));

    CHECK(svc.current_route_point_count() == 3);

    svc.end_route();
  }

  SECTION("append after resume continues from restored count") {
    // First session: 3 points.
    REQUIRE(svc.start_route(66666));
    for (int i = 0; i < 3; ++i) {
      REQUIRE(svc.append_route_point(make_route_point(i)));
    }
    svc.end_route();

    // Resume and append 2 more.
    REQUIRE(svc.start_route(66666));
    REQUIRE(svc.append_route_point(make_route_point(10)));
    REQUIRE(svc.append_route_point(make_route_point(11)));

    CHECK(svc.current_route_point_count() == 5);

    svc.end_route();
  }

  SECTION("full N+M cycle: file on disk contains N+M RoutePoints") {
    constexpr uint32_t SESSION_ID = 77777;
    const RoutePoint p0 = make_route_point(1);
    const RoutePoint p1 = make_route_point(2);
    const RoutePoint p2 = make_route_point(3); // written in second boot

    // First boot: write 2 points.
    REQUIRE(svc.start_route(SESSION_ID));
    REQUIRE(svc.append_route_point(p0));
    REQUIRE(svc.append_route_point(p1));
    svc.end_route();

    // Second boot (resume): write 1 more point.
    REQUIRE(svc.start_route(SESSION_ID));
    REQUIRE(svc.append_route_point(p2));
    svc.end_route();

    // Verify file contains exactly 3 RoutePoints.
    const std::string path = tmp / "routes/route_77777.bin";
    FILE *f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);

    RoutePoint pts[3]{};
    REQUIRE(fread(pts, sizeof(RoutePoint), 3, f) == 3);
    // No fourth point should exist.
    RoutePoint extra{};
    CHECK(fread(&extra, sizeof(RoutePoint), 1, f) == 0);
    fclose(f);

    CHECK(pts[0].timestamp == p0.timestamp);
    CHECK(pts[1].timestamp == p1.timestamp);
    CHECK(pts[2].timestamp == p2.timestamp);
  }
}
