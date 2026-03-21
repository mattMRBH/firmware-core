idf_component_register(
    SRCS "drivers/native_gpio.cpp"
    INCLUDE_DIRS "hal" "drivers"
    REQUIRES esp_driver_gpio
)
