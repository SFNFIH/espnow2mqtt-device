idf_component_register(
    SRCS "main.c" "../../../drivers/drv_gpio_relay.c" "../../../drivers/drv_gpio_button.c"
    INCLUDE_DIRS "." "../../../drivers"
    REQUIRES en2m driver esp_timer esp_hw_support
)
