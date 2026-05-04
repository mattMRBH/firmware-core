#ifndef SENSIRION_I2C_HAL_ESP_IDF_H
#define SENSIRION_I2C_HAL_ESP_IDF_H

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP-IDF specific hook to reuse an existing i2c_master bus.
void sensirion_i2c_hal_set_bus_handle(i2c_master_bus_handle_t bus_handle);

#ifdef __cplusplus
}
#endif

#endif // SENSIRION_I2C_HAL_ESP_IDF_H
