idf_component_register(
    SRCS "backends/nvs_config_store.cpp"
    INCLUDE_DIRS "hal" "backends"
    REQUIRES nvs_flash
)
