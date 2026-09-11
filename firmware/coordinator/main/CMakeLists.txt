idf_component_register(
    SRCS "main.c" "usb_host_link.c"
    INCLUDE_DIRS "."
    REQUIRES espnow2mqtt_mesh json driver esp_timer nvs_flash
)
