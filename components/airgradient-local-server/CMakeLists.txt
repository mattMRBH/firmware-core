idf_component_register(
    SRCS "internal/measures_json.cpp"
         "internal/config_json.cpp"
         "services/local_server.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common
             airgradient-http-server
             json
)
