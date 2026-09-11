idf_component_register(
    SRCS "main.c" "../../../drivers/drv_gpio_relay.c"
    INCLUDE_DIRS "." "../../../drivers"
    REQUIRES en2m driver nvs_flash
)
