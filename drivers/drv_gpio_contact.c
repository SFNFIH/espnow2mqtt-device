#include "drv_gpio_contact.h"

typedef struct {
    gpio_num_t pin;
    bool active_low;
} drv_gpio_contact_ctx_t;

static drv_gpio_contact_ctx_t s_contact;

esp_err_t drv_gpio_contact_init(gpio_num_t pin, bool active_low)
{
    s_contact.pin = pin;
    s_contact.active_low = active_low;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    return gpio_config(&io);
}

esp_err_t drv_gpio_contact_get(bool *open, void *ctx)
{
    (void)ctx;
    int level;
    if (open == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    level = gpio_get_level(s_contact.pin);
    /* open==true means door open / contact open */
    if (s_contact.active_low) {
        *open = (level == 0);
    } else {
        *open = (level != 0);
    }
    return ESP_OK;
}

esp_err_t drv_gpio_contact_watch(gpio_isr_t handler, void *arg)
{
    esp_err_t err;

    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = gpio_set_intr_type(s_contact.pin, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(s_contact.pin, handler, arg);
}
