#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t drv_gpio_contact_init(gpio_num_t pin, bool active_low);
esp_err_t drv_gpio_contact_get(bool *open, void *ctx);

#ifdef __cplusplus
}
#endif
