idf_component_register(
    SRCS "services/at_command_handler.cpp"
         "drivers/simcom_a7672x/simcom_a7672x.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common airgradient-gpio airgradient-serial
)
