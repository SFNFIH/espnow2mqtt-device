idf_component_register(
    SRCS "main.c" "../../../drivers/drv_gpio_contact.c"
    INCLUDE_DIRS "." "../../../drivers"
    REQUIRES en2m driver
)
