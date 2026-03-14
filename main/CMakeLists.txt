idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager mqtt_client_mod bus_decoder bus_writer
)
