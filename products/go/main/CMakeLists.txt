idf_component_register(
    SRCS "main.cpp"
         "go_types.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio
             esp_driver_i2c
             esp_driver_spi
             nvs_flash
)
