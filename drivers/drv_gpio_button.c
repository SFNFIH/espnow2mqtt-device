#include "drv_gpio_button.h"

#include "esp_timer.h"

typedef struct {
    gpio_num_t pin;
    bool active_low;
    int64_t debounce_us;
    int64_t last_us;
    drv_gpio_button_cb_t on_press;
    void *ctx;
} drv_gpio_button_t;

static drv_gpio_button_t s_button;

static void drv_gpio_button_isr(void *arg)
{
    drv_gpio_button_t *btn = (drv_gpio_button_t *)arg;
    int64_t now = esp_timer_get_time();

    if (now - btn->last_us < btn->debounce_us) {
        return;
    }
    btn->last_us = now;
    if (btn->on_press != NULL) {
        btn->on_press(btn->ctx);
    }
}

esp_err_t drv_gpio_button_init(gpio_num_t pin, bool active_low, uint32_t debounce_ms,
                               drv_gpio_button_cb_t on_press, void *ctx)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    esp_err_t err;

    if (on_press == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_button.pin = pin;
    s_button.active_low = active_low;
    s_button.debounce_us = (int64_t)debounce_ms * 1000;
    s_button.last_us = 0;
    s_button.on_press = on_press;
    s_button.ctx = ctx;

    err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(pin, drv_gpio_button_isr, &s_button);
}
