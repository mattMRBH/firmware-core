idf_component_register(
    SRCS "drivers/nimble_ble_server.cpp"
    INCLUDE_DIRS "." "drivers"
    REQUIRES esp-nimble-cpp
)
