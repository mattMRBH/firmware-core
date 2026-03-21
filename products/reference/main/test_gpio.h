#ifndef TEST_GPIO_H
#define TEST_GPIO_H

#include "airgradient_gpio.h"

// Exercises gpio::Hal: configure output, set_level blink, configure input,
// get_level read. Blinks PIN_LED_INDICATOR and reads PIN_BOOT_BUTTON.
void run_test_gpio(const gpio::Hal &hal);

#endif // TEST_GPIO_H
