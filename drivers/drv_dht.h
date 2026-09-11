#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t drv_dht_init(gpio_num_t pin, int dht_type);
esp_err_t drv_dht_get_temperature(int16_t *centi_celsius, void *ctx);
esp_err_t drv_dht_get_humidity(uint16_t *centi_percent, void *ctx);

#ifdef __cplusplus
}
#endif
