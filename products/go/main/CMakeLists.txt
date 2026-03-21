idf_component_register(
    SRCS "main.cpp"
         "go_types.cpp"
         "go_settings.cpp"
         "go_gps.cpp"
         "go_input.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common
             airgradient-config
             airgradient-gps
             airgradient-gpio
             airgradient-touch
             esp_driver_gpio
             esp_driver_i2c
             esp_driver_spi
             nvs_flash
)
