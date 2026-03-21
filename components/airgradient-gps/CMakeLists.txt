idf_component_register(
    SRCS "drivers/nmea_gps/nmea_gps.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-serial libnmea-esp32
)
