idf_component_register(
    SRCS "drivers/bq25xx/bq25xx.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common esp_driver_i2c
)
