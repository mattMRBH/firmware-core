/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <cstring>
#include <deque>
#include <string>

#if __has_include("rtos.h")
#include "rtos.h"
#else
#include "../../airgradient-common/include/rtos.h"
#endif

#include "../services/at_command_handler.h"

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
};

class ScopedRTOSInstance {
public:
  explicit ScopedRTOSInstance(RTOS *rtos) { RTOS::set_instance(rtos); }

  ~ScopedRTOSInstance() { RTOS::set_instance(nullptr); }
};

class MockAirgradientSerial : public AirgradientSerial {
public:
  MAKE_MOCK1(begin, bool(int), override);
  MAKE_MOCK0(end, void(), override);
  MAKE_MOCK0(available, int(), override);
  MAKE_MOCK1(print, void(const char *), override);
  MAKE_MOCK2(write, int(const uint8_t *, int), override);
  MAKE_MOCK0(read, int(), override);
  MAKE_MOCK2(read, int(uint8_t *, int), override);

  void queue_rx(const char *data) {
    while (data != nullptr && *data != '\0') {
      _rx.push_back(static_cast<uint8_t>(*data));
      data++;
    }
  }

  int queued_rx_size() const { return static_cast<int>(_rx.size()); }

  int pop_rx() {
    if (_rx.empty()) {
      return -1;
    }

    const uint8_t value = _rx.front();
    _rx.pop_front();
    return value;
  }

  void append_tx(const char *data) {
    if (data != nullptr) {
      _tx += data;
    }
  }

  void append_tx_bytes(const uint8_t *data, int len) {
    for (int index = 0; index < len; ++index) {
      _tx.push_back(static_cast<char>(data[index]));
    }
  }

  const std::string &tx() const { return _tx; }

private:
  std::deque<uint8_t> _rx;
  std::string _tx;
};

TEST_CASE("at_command_handler sends AT-prefixed commands",
          "[airgradient-cellular][at-command-handler]") {
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  MockAirgradientSerial serial;
  ATCommandHandler handler(serial);
  trompeloeil::sequence seq;

  REQUIRE_CALL(serial, print(trompeloeil::_))
      .WITH(std::strcmp(_1, "AT") == 0)
      .IN_SEQUENCE(seq)
      .LR_SIDE_EFFECT(serial.append_tx(_1));
  REQUIRE_CALL(serial, print(trompeloeil::_))
      .WITH(std::strcmp(_1, "+CPIN?") == 0)
      .IN_SEQUENCE(seq)
      .LR_SIDE_EFFECT(serial.append_tx(_1));
  REQUIRE_CALL(serial, print(trompeloeil::_))
      .WITH(std::strcmp(_1, "\r\n") == 0)
      .IN_SEQUENCE(seq)
      .LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));

  handler.send_at("+CPIN?");

  REQUIRE(serial.tx() == "AT+CPIN?\r\n");
}

TEST_CASE("at_command_handler matches response then reads remaining line",
          "[airgradient-cellular][at-command-handler]") {
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  MockAirgradientSerial serial;
  ATCommandHandler handler(serial);
  char line[16] = {};
  size_t line_length = 0;

  serial.queue_rx("\r\n+CPIN: READY\r\n");

  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_SIDE_EFFECT({}).LR_RETURN(serial.pop_rx());

  const auto response = handler.wait_response(100, "+CPIN:");
  const auto status = handler.wait_and_read_line({line, sizeof(line)}, &line_length);

  REQUIRE(response == ATCommandHandler::Response::Expected1);
  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(line_length == 5);
  REQUIRE(std::string(line) == "READY");
}

TEST_CASE("at_command_handler reports modem CMx errors",
          "[airgradient-cellular][at-command-handler]") {
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  MockAirgradientSerial serial;
  ATCommandHandler handler(serial);

  serial.queue_rx("\r\n+CME ERROR: 515\r\n");

  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_SIDE_EFFECT({}).LR_RETURN(serial.pop_rx());

  const auto response = handler.wait_response(100);

  REQUIRE(response == ATCommandHandler::Response::CmxError);
}

TEST_CASE("at_command_handler retrieves exact buffer length",
          "[airgradient-cellular][at-command-handler]") {
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  MockAirgradientSerial serial;
  ATCommandHandler handler(serial);
  uint8_t output[4] = {};
  size_t bytes_read = 0;

  serial.queue_rx("ABCD");

  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_SIDE_EFFECT({}).LR_RETURN(serial.pop_rx());

  const auto status =
      handler.retrieve_buffer({output, sizeof(output)}, sizeof(output), &bytes_read, 100);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(bytes_read == sizeof(output));
  REQUIRE(output[0] == static_cast<uint8_t>('A'));
  REQUIRE(output[1] == static_cast<uint8_t>('B'));
  REQUIRE(output[2] == static_cast<uint8_t>('C'));
  REQUIRE(output[3] == static_cast<uint8_t>('D'));
}
