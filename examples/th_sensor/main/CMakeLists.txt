idf_component_register(
    SRCS "main.c" "../../drivers/drv_dht.c"
    INCLUDE_DIRS "." "../../drivers"
    REQUIRES en2m driver esp_timer nvs_flash
)
