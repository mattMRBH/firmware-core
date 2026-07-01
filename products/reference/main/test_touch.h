#ifndef TEST_TOUCH_H
#define TEST_TOUCH_H

#include "driver/i2c_master.h"

// Exercises CAP1203 capacitive touch sensor: init (at default address 0x28),
// then polls touch + noise status for N_READINGS cycles.
// Returns early with a warning if the sensor is not detected on the I2C bus.
void run_test_touch(i2c_master_bus_handle_t i2c_bus);

#endif // TEST_TOUCH_H
