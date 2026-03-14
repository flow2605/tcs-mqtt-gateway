idf_component_register(SRCS "wifi_manager.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "../../main"
                       REQUIRES esp_wifi esp_event nvs_flash)
