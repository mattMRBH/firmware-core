idf_component_register(
    SRCS "main.cpp"
         "go_types.cpp"
         "go_settings.cpp"
         "go_display.cpp"
         "go_gps.cpp"
         "go_input.cpp"
         "go_power.cpp"
         "go_sensor_producer.cpp"
         "go_storage.cpp"
         "go_ui.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common
             airgradient-config
             airgradient-gps
             airgradient-gpio
             airgradient-nand-storage
             airgradient-payload-cache
             airgradient-sensors
             airgradient-touch
             esp_driver_gpio
             esp_driver_i2c
             esp_driver_spi
             nvs_flash
             u8g2
)
