idf_component_register(
    SRCS "rtos.cpp" "common.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer airgradient-gpio esp_app_format
)
