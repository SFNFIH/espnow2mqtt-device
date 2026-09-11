idf_component_register(
    SRCS "main.c" "../../../drivers/drv_gpio_button_gesture.c"
    INCLUDE_DIRS "." "../../../drivers"
    REQUIRES en2m driver
)
