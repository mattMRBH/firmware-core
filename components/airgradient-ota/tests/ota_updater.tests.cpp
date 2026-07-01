/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <algorithm>
#include <deque>
#include <vector>

#include "rtos.h"

#include "services/ota_updater.h"
#include "types/ota_types.h"

#include "fake_ota_image_writer.h"
#include "mock_ota_image_source.h"

using trompeloeil::_;

namespace {

// Host fake clock. Pops queued timestamps for get_time_ms(); when the queue is
// drained it keeps returning the last value. An empty queue yields 0 so the
// download loop never crosses the throttle interval (only the immediate
// Downloading is emitted).
class FakeRTOS : public RTOS {
public:
  void delay_ms_impl(uint32_t ms) override { (void)ms; }

  uint64_t get_time_ms_impl() override {
    if (!_times.empty()) {
      _last = _times.front();
      _times.pop_front();
    }
    return _last;
  }

  std::deque<uint64_t> _times;
  uint64_t _last = 0;
};

class ScopedRTOSInstance {
public:
  explicit ScopedRTOSInstance(RTOS *rtos) { RTOS::set_instance(rtos); }
  ~ScopedRTOSInstance() { RTOS::set_instance(nullptr); }
};

int count_state(const std::vector<OtaProgress> &events, OtaState state) {
  return static_cast<int>(std::count_if(
      events.begin(), events.end(), [state](const OtaProgress &p) { return p.state == state; }));
}

} // namespace

TEST_CASE("OtaUpdater happy path: open -> begin -> write loop -> finish", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  trompeloeil::sequence seq;
  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 200).RETURN(OtaStatus::Ok).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(100).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(100).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(0).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, close()).IN_SEQUENCE(seq);

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::Ok);
  REQUIRE(writer.begin_called);
  REQUIRE(writer.begin_total == 200);
  REQUIRE(writer.bytes_written() == 200);
  REQUIRE(writer.finish_called);
  REQUIRE(writer.abort_calls == 0);

  REQUIRE(events.front().state == OtaState::Checking);
  REQUIRE(events.back().state == OtaState::Done);
  REQUIRE(count_state(events, OtaState::Applying) == 1);
  REQUIRE(events.back().percent == 100);
}

TEST_CASE("OtaUpdater emits an immediate Downloading for a single-chunk image", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  trompeloeil::sequence seq;
  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 50).RETURN(OtaStatus::Ok).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(50).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(0).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, close()).IN_SEQUENCE(seq);

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::Ok);
  REQUIRE(writer.bytes_written() == 50);
  REQUIRE(count_state(events, OtaState::Downloading) >= 1);
}

TEST_CASE("OtaUpdater progress is throttled during the download loop", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  // last_cb=0, iter1=0 (no emit), iter2=1000 (emit), iter3=1000 (no emit).
  rtos._times = {0, 0, 1000, 1000};
  ScopedRTOSInstance scoped(&rtos);

  trompeloeil::sequence seq;
  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 300).RETURN(OtaStatus::Ok).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(100).TIMES(3).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(0).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, close()).IN_SEQUENCE(seq);

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::Ok);
  // One immediate Downloading after begin() + one throttled emission in-loop.
  REQUIRE(count_state(events, OtaState::Downloading) == 2);
}

TEST_CASE("OtaUpdater short-circuits UpToDate to a Skipped terminal", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  trompeloeil::sequence seq;
  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 0).RETURN(OtaStatus::UpToDate).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, close()).IN_SEQUENCE(seq);

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::UpToDate);
  REQUIRE_FALSE(writer.begin_called);
  REQUIRE(events.back().state == OtaState::Skipped);
}

TEST_CASE("OtaUpdater short-circuits Declined to a Skipped terminal", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 0).RETURN(OtaStatus::Declined);
  REQUIRE_CALL(source, close());

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::Declined);
  REQUIRE_FALSE(writer.begin_called);
  REQUIRE(events.back().state == OtaState::Skipped);
}

TEST_CASE("OtaUpdater reports a Failed terminal when open errors", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 0).RETURN(OtaStatus::TransportError);
  REQUIRE_CALL(source, close());

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::TransportError);
  REQUIRE_FALSE(writer.begin_called);
  REQUIRE(events.back().state == OtaState::Failed);
}

TEST_CASE("OtaUpdater fails and closes when begin() fails", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  writer.begin_status = OtaStatus::FlashError;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 200).RETURN(OtaStatus::Ok);
  REQUIRE_CALL(source, close());

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::FlashError);
  REQUIRE(writer.abort_calls == 0); // begin failed; nothing to abort
  REQUIRE(events.back().state == OtaState::Failed);
}

TEST_CASE("OtaUpdater aborts the writer on a read error", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  trompeloeil::sequence seq;
  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 200).RETURN(OtaStatus::Ok).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(-1).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, close()).IN_SEQUENCE(seq);

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::TransportError);
  REQUIRE(writer.abort_calls == 1);
  REQUIRE_FALSE(writer.finish_called);
  REQUIRE(events.back().state == OtaState::Failed);
}

TEST_CASE("OtaUpdater aborts the writer on a write/flash error", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  writer.write_status = OtaStatus::FlashError;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  // total unknown (0) so the truncation guard does not mask the write failure.
  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 0).RETURN(OtaStatus::Ok);
  REQUIRE_CALL(source, read(_, _)).RETURN(100);
  REQUIRE_CALL(source, close());

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::FlashError);
  REQUIRE(writer.abort_calls == 1);
  REQUIRE_FALSE(writer.finish_called);
  REQUIRE(events.back().state == OtaState::Failed);
}

TEST_CASE("OtaUpdater guards against a truncated download", "[ota_updater]") {
  MockOtaImageSource source;
  FakeOtaImageWriter writer;
  FakeRTOS rtos;
  ScopedRTOSInstance scoped(&rtos);

  // Declares 200 bytes but only delivers 100 before EOF.
  trompeloeil::sequence seq;
  REQUIRE_CALL(source, open(_)).SIDE_EFFECT(*_1 = 200).RETURN(OtaStatus::Ok).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(100).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, read(_, _)).RETURN(0).IN_SEQUENCE(seq);
  REQUIRE_CALL(source, close()).IN_SEQUENCE(seq);

  OtaUpdater updater(source, writer);
  std::vector<OtaProgress> events;
  updater.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  const OtaStatus st = updater.run();

  REQUIRE(st == OtaStatus::TransportError);
  REQUIRE(writer.abort_calls == 1);
  REQUIRE_FALSE(writer.finish_called);
  REQUIRE(events.back().state == OtaState::Failed);
}
