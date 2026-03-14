idf_component_register(SRCS "mqtt_client_mod.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "../../main"
                       REQUIRES mqtt esp_event
                       EMBED_TXTFILES "hivemq_ca.pem"
                       )               