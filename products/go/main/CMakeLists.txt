idf_component_register(
    SRCS "main.cpp"
         "go_app.cpp"
         "go_hardware_board.cpp"
         "go_ble.cpp"
         "go_cloud.cpp"
         "go_types.cpp"
         "go_settings.cpp"
         "go_display.cpp"
         "gps/gps_driver.cpp"
         "gps/gps_service.cpp"
         "go_input.cpp"
         "go_orchestrator.cpp"
         "go_power.cpp"
         "go_sensor_producer.cpp"
         "go_storage.cpp"
         "go_ui.cpp"
         "go_ulp.cpp"
         "text_wrap.cpp"
         "wifi_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-bms
             bq25629
    REQUIRES airgradient-ble
             airgradient-client
             airgradient-common
             airgradient-config
              airgradient-gpio
              airgradient-http-server
              airgradient-nand-storage
              airgradient-provisioning
              airgradient-serial
              airgradient-wifi
              libnmea-esp32
             airgradient-payload-cache
             airgradient-sensors
             airgradient-touch
             esp_driver_gpio
             esp_driver_i2c
             esp_driver_spi
             nvs_flash
             u8g2
             fatfs
             ulp
)

ulp_embed_binary(ulp_wdt "ulp/wdt_feed.c" "")
