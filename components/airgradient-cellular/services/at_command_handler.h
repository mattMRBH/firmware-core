/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AT_COMMAND_HANDLER_H
#define AT_COMMAND_HANDLER_H

#include <cstddef>
#include <cstdint>

#if __has_include("airgradient_serial.h")
#include "airgradient_serial.h"
#else
#include "../../airgradient-serial/include/airgradient_serial.h"
#endif

#include "../types/cellular_types.h"

class ATCommandHandler {
public:
  enum class Response : uint8_t {
    Expected1,
    Expected2,
    Expected3,
    Timeout,
    CmxError,
    BufferOverflow,
  };

  static constexpr uint32_t DEFAULT_TEST_AT_TIMEOUT_MS = 60000;
  static constexpr uint32_t DEFAULT_WAIT_RESPONSE_TIMEOUT_MS = 9000;
  static constexpr uint32_t DEFAULT_READ_LINE_TIMEOUT_MS = 3000;
  static constexpr uint32_t DEFAULT_RETRIEVE_BUFFER_TIMEOUT_MS = 3000;
  static constexpr uint32_t WRITE_YIELD_DELAY_MS = 2;
  static constexpr uint32_t POLL_DELAY_MS = 2;
  static constexpr size_t RX_BUFFER_SIZE = 4096;
  static constexpr const char *RESPONSE_OK = "OK\r\n";
  static constexpr const char *RESPONSE_ERROR = "ERROR\r\n";
  static constexpr const char *RESPONSE_CME_ERROR = "+CME ERROR:";
  static constexpr const char *RESPONSE_CMS_ERROR = "+CMS ERROR:";

  explicit ATCommandHandler(AirgradientSerial &serial);

  bool test_at(uint32_t timeout_ms = DEFAULT_TEST_AT_TIMEOUT_MS);

  void send_at(const char *command);
  void send_raw(const char *command);
  void send_raw(const uint8_t *data, size_t size);

  Response wait_response(uint32_t timeout_ms, const char *expected_1 = RESPONSE_OK,
                         const char *expected_2 = RESPONSE_ERROR, const char *expected_3 = nullptr);
  Response wait_response(const char *expected_1 = RESPONSE_OK,
                         const char *expected_2 = RESPONSE_ERROR, const char *expected_3 = nullptr);

  CellularStatus wait_and_read_line(MutableStringBuffer out_line, size_t *out_length = nullptr,
                                    uint32_t timeout_ms = DEFAULT_READ_LINE_TIMEOUT_MS,
                                    bool exclude_leading_space = true);

  CellularStatus retrieve_buffer(MutableBuffer out_buffer, size_t expected_length,
                                 size_t *out_length = nullptr,
                                 uint32_t timeout_ms = DEFAULT_RETRIEVE_BUFFER_TIMEOUT_MS);

  void clear_buffer();

private:
  bool _ends_with(const char *target) const;
  void _reset_rx_buffer();

  AirgradientSerial &_serial;
  char _rx_buffer[RX_BUFFER_SIZE] = {};
  size_t _rx_buffer_length = 0;
};

#endif // AT_COMMAND_HANDLER_H
