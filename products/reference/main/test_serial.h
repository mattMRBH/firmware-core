#ifndef TEST_SERIAL_H
#define TEST_SERIAL_H

#include "driver/i2c_master.h"

// Exercises AirgradientUART and AirgradientIICSerial: verifies begin() on
// both transport implementations. Uses the board's CO2 UART and IIC-UART
// bridge channels defined in board_config.h.
void run_test_serial(i2c_master_bus_handle_t i2c_bus);

#endif // TEST_SERIAL_H
