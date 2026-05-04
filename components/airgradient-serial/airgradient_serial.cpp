/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "airgradient_serial.h"

AirgradientSerial::AirgradientSerial() {}

AirgradientSerial::~AirgradientSerial() {}

bool AirgradientSerial::begin(int baud) { return false; }

void AirgradientSerial::end() {}

int AirgradientSerial::available() { return 0; }

void AirgradientSerial::print(const char *str) {}

int AirgradientSerial::write(const uint8_t *data, int len) { return 0; }

int AirgradientSerial::read() { return -1; }

int AirgradientSerial::read(uint8_t *buf, int len) {
  if (buf == nullptr || len <= 0) {
    return 0;
  }
  int written = 0;
  for (int i = 0; i < len; i++) {
    const int b = read();
    if (b < 0) {
      break;
    }
    buf[written++] = static_cast<uint8_t>(b);
  }
  return written;
}

void AirgradientSerial::setDebug(bool debug) { isDebug = debug; }
