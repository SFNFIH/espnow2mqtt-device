#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t drv_gpio_contact_init(gpio_num_t pin, bool active_low);
esp_err_t drv_gpio_contact_get(bool *open, void *ctx);

/**
 * @brief Call @p handler on every edge of the contact input.
 *
 * Installs the shared GPIO ISR service if the application has not done so.
 * The handler runs in interrupt context, so it should only defer work
 * (see en2m_schedule_from_isr / en2m_attribute_set_from_isr).
 */
esp_err_t drv_gpio_contact_watch(gpio_isr_t handler, void *arg);

#ifdef __cplusplus
}
#endif
