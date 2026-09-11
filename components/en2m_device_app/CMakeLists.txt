idf_component_register(
    SRCS "src/en2m_device_app.c"
    INCLUDE_DIRS "include"
    REQUIRES espnow2mqtt_mesh json driver esp_timer nvs_flash
)
