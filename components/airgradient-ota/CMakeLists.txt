idf_component_register(
    SRCS "services/ota_updater.cpp"
         "services/ota_url.cpp"
         "backends/esp/esp_ota_image_writer.cpp"
         "backends/wifi/wifi_http_ota_source.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common app_update esp_http_client
)
