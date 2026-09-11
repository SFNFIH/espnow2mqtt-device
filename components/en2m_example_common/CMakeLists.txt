idf_component_register(
    SRCS "src/en2m_example_common.c"
    INCLUDE_DIRS "include"
    REQUIRES espnow2mqtt_mesh driver esp_timer
)
