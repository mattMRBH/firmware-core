idf_component_register(
    SRCS "services/payload_cache.cpp"
         "backends/rtc_payload_cache_storage.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common esp_common
)
