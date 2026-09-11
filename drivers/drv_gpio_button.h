#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Invoked in interrupt context for every accepted press. */
typedef void (*drv_gpio_button_cb_t)(void *ctx);

/**
 * @brief Watch a momentary push button and debounce it in the driver.
 *
 * @param pin         Button input, pulled to the idle level internally.
 * @param active_low  True when pressing pulls the pin to ground.
 * @param debounce_ms Presses closer together than this are ignored.
 * @param on_press    Runs in interrupt context; defer real work from it.
 */
esp_err_t drv_gpio_button_init(gpio_num_t pin, bool active_low, uint32_t debounce_ms,
                               drv_gpio_button_cb_t on_press, void *ctx);

#ifdef __cplusplus
}
#endif
