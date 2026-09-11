#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t drv_gpio_relay_init(gpio_num_t pin, bool active_high);
esp_err_t drv_gpio_relay_set(bool on, void *ctx);
esp_err_t drv_gpio_relay_get(bool *on, void *ctx);

#ifdef __cplusplus
}
#endif
