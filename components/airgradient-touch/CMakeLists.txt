idf_component_register(
    SRCS "drivers/cap1203.cpp"
    INCLUDE_DIRS "hal" "drivers"
    PRIV_REQUIRES esp_driver_i2c
)
