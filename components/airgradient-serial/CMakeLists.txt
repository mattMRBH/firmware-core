idf_component_register(SRCS
                       "airgradient_serial.cpp"
                       "airgradient_uart.cpp"
                       "airgradient_iic_serial.cpp"
                       "vendor/wk2132_driver.cpp" 
		       INCLUDE_DIRS "include" "vendor"
		       REQUIRES esp_driver_uart esp_driver_i2c esp_driver_gpio esp_timer)
