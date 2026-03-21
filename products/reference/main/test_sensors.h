#ifndef TEST_SENSORS_H
#define TEST_SENSORS_H

#include "driver/i2c_master.h"

// Exercises PMS5003 (dual channel via IIC bridge), SHT40, SGP41, and Sunlight
// CO2 sensor. Runs N_READINGS poll cycles with a 2 s interval and logs all
// valid fields. Returns after the last cycle; does not loop indefinitely.
void run_test_sensors(i2c_master_bus_handle_t i2c_bus);

#endif  // TEST_SENSORS_H
