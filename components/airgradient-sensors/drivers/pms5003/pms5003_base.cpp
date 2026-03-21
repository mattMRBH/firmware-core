/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/pms5003/pms5003_base.h"
#include "esp_log.h"
#include "rtos.h"
#include <cstring>

static constexpr const char *TAG = "PMS5003Base";

PMS5003Base::PMS5003Base(AirgradientSerial &serial)
    : _serial(serial), _mode(Mode::ACTIVE), _index(0), _frame_len(0),
      _checksum(0), _calculated_checksum(0) {
  memset(_payload, 0, PAYLOAD_SIZE);
}

bool PMS5003Base::init() {
  // Serial should already be initialized by caller
  // Just verify we can communicate
  passive_mode();
  RTOS::delay_ms(100);
  return is_connected();
}

void PMS5003Base::sleep() {
  uint8_t command[] = {0x42, 0x4D, 0xE4, 0x00, 0x00, 0x01, 0x73};
  _serial.write(command, sizeof(command));
}

void PMS5003Base::wake_up() {
  uint8_t command[] = {0x42, 0x4D, 0xE4, 0x00, 0x01, 0x01, 0x74};
  _serial.write(command, sizeof(command));
}

void PMS5003Base::active_mode() {
  uint8_t command[] = {0x42, 0x4D, 0xE1, 0x00, 0x01, 0x01, 0x71};
  _serial.write(command, sizeof(command));
  _mode = Mode::ACTIVE;
}

void PMS5003Base::passive_mode() {
  uint8_t command[] = {0x42, 0x4D, 0xE1, 0x00, 0x00, 0x01, 0x70};
  _serial.write(command, sizeof(command));
  _mode = Mode::PASSIVE;
}

bool PMS5003Base::is_connected() {
  for (int i = 0; i < 3; i++) {
    clear_buffer();
    request_read();
    if (read_frame(1000)) {
      return true;
    }
    RTOS::delay_ms(200);
  }
  return false;
}

void PMS5003Base::clear_buffer() {
  int bytes_cleared = 0;
  while (_serial.available()) {
    _serial.read();
    bytes_cleared++;
  }
  ESP_LOGD(TAG, "Cleared %d byte(s)", bytes_cleared);
}

void PMS5003Base::request_read() {
  if (_mode == Mode::PASSIVE) {
    uint8_t command[] = {0x42, 0x4D, 0xE2, 0x00, 0x00, 0x01, 0x71};
    _serial.write(command, sizeof(command));
  }
}

bool PMS5003Base::read_frame(uint16_t timeout_ms) {
  uint64_t start = RTOS::get_time_ms();
  bool frame_ready = false;

  _index = 0; // Reset state machine

  do {
    if (_process_byte()) {
      frame_ready = true;
      break;
    }
  } while ((RTOS::get_time_ms() - start) < timeout_ms);

  return frame_ready;
}

bool PMS5003Base::_process_byte() {
  if (!_serial.available()) {
    return false;
  }

  uint8_t ch = _serial.read();
  ESP_LOGV(TAG, "%.2x", ch);

  switch (_index) {
  case 0:
    // Fixed start character 0x42
    if (ch != 0x42) {
      return false;
    }
    _calculated_checksum = ch;
    break;

  case 1:
    // Fixed second character 0x4D
    if (ch != 0x4D) {
      _index = 0;
      return false;
    }
    _calculated_checksum += ch;
    break;

  case 2:
    // High byte of frame length
    _calculated_checksum += ch;
    _frame_len = ch << 8;
    break;

  case 3:
    // Low byte of frame length
    _frame_len |= ch;
    // Valid frame lengths: 2*9+2=20, 2*13+2=28, 2*17+2=36
    if (_frame_len != 20 && _frame_len != 28 && _frame_len != 36) {
      _index = 0;
      return false;
    }
    _calculated_checksum += ch;
    break;

  default:
    // Checksum high byte
    if (_index == _frame_len + 2) {
      _checksum = ch << 8;
    }
    // Checksum low byte
    else if (_index == _frame_len + 2 + 1) {
      _checksum |= ch;

      // Validate checksum
      if (_calculated_checksum == _checksum) {
        _index = 0;
        return true; // Valid frame complete
      }

      _index = 0;
      return false;
    }
    // Payload byte
    else {
      _calculated_checksum += ch;
      uint8_t payload_index = _index - 4;

      if (payload_index < PAYLOAD_SIZE) {
        _payload[payload_index] = ch;
      }
    }
    break;
  }

  _index++;
  return false;
}

uint16_t PMS5003Base::_make_word(uint8_t high, uint8_t low) const {
  return (uint16_t)((high << 8) | low);
}
