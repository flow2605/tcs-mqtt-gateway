idf_component_register(
    SRCS "bus_writer.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer freertos
)
