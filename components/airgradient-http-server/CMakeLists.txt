idf_component_register(
    SRCS "drivers/idf_http_server.cpp" "types/http_types.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common esp_http_server
)
