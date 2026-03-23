#ifndef TEST_SENSORS_H
#define TEST_SENSORS_H

#include "driver/i2c_master.h"

// Exercises SPS30 (PM via I2C), STCC4 (CO2 + temp/hum via I2C), SGP41,
// and DPS368 (pressure + altitude via I2C).
// Runs N_READINGS poll cycles with a 2 s interval and logs all valid fields.
// Returns after the last cycle; does not loop indefinitely.
void run_test_sensors(i2c_master_bus_handle_t i2c_bus);

#endif // TEST_SENSORS_H
