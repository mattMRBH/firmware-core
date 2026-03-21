idf_component_register(
    SRCS "drivers/sht40/sht40.cpp"
         "drivers/pms5003/pms5003_base.cpp"
         "drivers/pms5003/pms5003.cpp"
         "drivers/pms5003/pms5003t.cpp"
         "drivers/co2_common/modbus_crc.cpp"
         "drivers/s8/s8.cpp"
         "drivers/sunlight/sunlight.cpp"
         "drivers/sgp41/sgp41.cpp"
         "drivers/bq25xx/bq25xx.cpp"
         "drivers/alpha_sense/alpha_sense.cpp"
         "services/sensor_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common airgradient-serial ads1115 esp_driver_gpio esp_driver_i2c
)
