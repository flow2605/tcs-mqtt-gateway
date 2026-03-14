idf_component_register(
    SRCS "bus_decoder.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos
)
