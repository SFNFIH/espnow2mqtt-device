#include "drv_gpio_relay.h"

#include "nvs.h"

typedef struct {
    gpio_num_t pin;
    bool active_high;
    bool on;
} drv_gpio_relay_ctx_t;

static drv_gpio_relay_ctx_t s_relay;

esp_err_t drv_gpio_relay_init(gpio_num_t pin, bool active_high)
{
    s_relay.pin = pin;
    s_relay.active_high = active_high;
    s_relay.on = false;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    nvs_handle_t h;
    if (nvs_open("drv_relay", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "on", &v) == ESP_OK) {
            s_relay.on = v != 0;
        }
        nvs_close(h);
    }
    gpio_set_level(pin, (s_relay.on == s_relay.active_high) ? 1 : 0);
    return ESP_OK;
}

esp_err_t drv_gpio_relay_set(bool on, void *ctx)
{
    (void)ctx;
    s_relay.on = on;
    gpio_set_level(s_relay.pin, (on == s_relay.active_high) ? 1 : 0);
    nvs_handle_t h;
    if (nvs_open("drv_relay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "on", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

esp_err_t drv_gpio_relay_get(bool *on, void *ctx)
{
    (void)ctx;
    if (on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *on = s_relay.on;
    return ESP_OK;
}
