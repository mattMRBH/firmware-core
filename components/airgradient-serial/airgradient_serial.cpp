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

void AirgradientSerial::setDebug(bool debug) { isDebug = debug; }
