idf_component_register(
    SRCS "rtos.cpp" "common.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_log esp_timer airgradient-gpio
)
