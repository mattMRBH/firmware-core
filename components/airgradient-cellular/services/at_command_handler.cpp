/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "at_command_handler.h"

#include <cstring>

#if __has_include("rtos.h")
#include "rtos.h"
#else
#include "../../airgradient-common/include/rtos.h"
#endif

ATCommandHandler::ATCommandHandler(AirgradientSerial &serial)
    : _serial(serial) {}

bool ATCommandHandler::test_at(uint32_t timeout_ms) {
  const uint64_t start_time_ms = RTOS::get_time_ms();

  while ((RTOS::get_time_ms() - start_time_ms) < timeout_ms) {
    send_raw("AT");
    if (wait_response(500) == Response::Expected1) {
      return true;
    }
    RTOS::delay_ms(100);
  }

  return false;
}

void ATCommandHandler::send_at(const char *command) {
  _serial.print("AT");
  if (command != nullptr) {
    _serial.print(command);
  }
  _serial.print("\r\n");
  RTOS::delay_ms(WRITE_YIELD_DELAY_MS);
}

void ATCommandHandler::send_raw(const char *command) {
  if (command != nullptr) {
    _serial.print(command);
  }
  _serial.print("\r\n");
  RTOS::delay_ms(WRITE_YIELD_DELAY_MS);
}

void ATCommandHandler::send_raw(const uint8_t *data, size_t size) {
  if (data != nullptr && size > 0) {
    _serial.write(data, static_cast<int>(size));
  }
  _serial.print("\r\n");
  RTOS::delay_ms(WRITE_YIELD_DELAY_MS);
}

ATCommandHandler::Response
ATCommandHandler::wait_response(uint32_t timeout_ms, const char *expected_1,
                                const char *expected_2,
                                const char *expected_3) {
  _reset_rx_buffer();

  const uint64_t start_time_ms = RTOS::get_time_ms();

  while ((RTOS::get_time_ms() - start_time_ms) < timeout_ms) {
    while (_serial.available() > 0) {
      if (_rx_buffer_length >= (RX_BUFFER_SIZE - 1)) {
        _rx_buffer[RX_BUFFER_SIZE - 1] = '\0';
        return Response::BufferOverflow;
      }

      const int value = _serial.read();
      if (value < 0) {
        break;
      }

      _rx_buffer[_rx_buffer_length] = static_cast<char>(value);
      _rx_buffer_length++;
      _rx_buffer[_rx_buffer_length] = '\0';

      if (expected_1 != nullptr && _ends_with(expected_1)) {
        return Response::Expected1;
      }
      if (expected_2 != nullptr && _ends_with(expected_2)) {
        return Response::Expected2;
      }
      if (expected_3 != nullptr && _ends_with(expected_3)) {
        return Response::Expected3;
      }
      if (_ends_with(RESPONSE_CME_ERROR) || _ends_with(RESPONSE_CMS_ERROR)) {
        char error_line[32] = {};
        wait_and_read_line({error_line, sizeof(error_line)});
        return Response::CmxError;
      }
    }

    RTOS::delay_ms(POLL_DELAY_MS);
  }

  return Response::Timeout;
}

ATCommandHandler::Response
ATCommandHandler::wait_response(const char *expected_1, const char *expected_2,
                                const char *expected_3) {
  return wait_response(DEFAULT_WAIT_RESPONSE_TIMEOUT_MS, expected_1, expected_2,
                       expected_3);
}

CellularStatus
ATCommandHandler::wait_and_read_line(MutableStringBuffer out_line,
                                     size_t *out_length, uint32_t timeout_ms,
                                     bool exclude_leading_space) {
  if (out_line.data == nullptr || out_line.size == 0) {
    return CellularStatus::InvalidArgument;
  }

  out_line.data[0] = '\0';
  size_t index = 0;
  const uint64_t start_time_ms = RTOS::get_time_ms();

  while ((RTOS::get_time_ms() - start_time_ms) < timeout_ms) {
    while (_serial.available() > 0) {
      const int value = _serial.read();
      if (value < 0) {
        break;
      }

      const char current = static_cast<char>(value);
      if (exclude_leading_space && index == 0 && current == ' ') {
        continue;
      }

      if (current == '\r') {
        const int newline = _serial.read();
        if (newline == '\n') {
          if (out_length != nullptr) {
            *out_length = index;
          }
          return CellularStatus::Ok;
        }
      }

      if ((index + 1) >= out_line.size) {
        out_line.data[out_line.size - 1] = '\0';
        if (out_length != nullptr) {
          *out_length = index;
        }
        return CellularStatus::BufferTooSmall;
      }

      out_line.data[index] = current;
      index++;
      out_line.data[index] = '\0';
    }

    RTOS::delay_ms(POLL_DELAY_MS);
  }

  if (out_length != nullptr) {
    *out_length = index;
  }
  return CellularStatus::Timeout;
}

CellularStatus ATCommandHandler::retrieve_buffer(MutableBuffer out_buffer,
                                                 size_t expected_length,
                                                 size_t *out_length,
                                                 uint32_t timeout_ms) {
  if (expected_length > 0 && out_buffer.data == nullptr) {
    return CellularStatus::InvalidArgument;
  }
  if (expected_length > out_buffer.size) {
    return CellularStatus::BufferTooSmall;
  }

  size_t index = 0;
  const uint64_t start_time_ms = RTOS::get_time_ms();

  while ((RTOS::get_time_ms() - start_time_ms) < timeout_ms &&
         index < expected_length) {
    while (_serial.available() > 0 && index < expected_length) {
      const int value = _serial.read();
      if (value < 0) {
        break;
      }

      out_buffer.data[index] = static_cast<uint8_t>(value);
      index++;
    }

    if (index >= expected_length) {
      break;
    }

    RTOS::delay_ms(POLL_DELAY_MS);
  }

  if (out_length != nullptr) {
    *out_length = index;
  }
  if (index == expected_length) {
    return CellularStatus::Ok;
  }
  return CellularStatus::Timeout;
}

void ATCommandHandler::clear_buffer() {
  while (_serial.available() > 0) {
    _serial.read();
  }
}

bool ATCommandHandler::_ends_with(const char *target) const {
  if (target == nullptr) {
    return false;
  }

  const size_t target_length = std::strlen(target);
  if (target_length > _rx_buffer_length) {
    return false;
  }

  return std::strncmp(_rx_buffer + _rx_buffer_length - target_length, target,
                      target_length) == 0;
}

void ATCommandHandler::_reset_rx_buffer() {
  _rx_buffer[0] = '\0';
  _rx_buffer_length = 0;
}
