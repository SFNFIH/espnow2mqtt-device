#include "drv_dht.h"

#include "esp_timer.h"
#include "rom/ets_sys.h"

typedef struct {
    gpio_num_t pin;
    int dht_type;
    int16_t last_temp_centi;
    uint16_t last_hum_centi;
} drv_dht_ctx_t;

static drv_dht_ctx_t s_dht;

esp_err_t drv_dht_init(gpio_num_t pin, int dht_type)
{
    s_dht.pin = pin;
    s_dht.dht_type = dht_type;
    return ESP_OK;
}

static bool drv_dht_sample(void)
{
    uint8_t data[5] = {0};
    int gpio_num = s_dht.pin;
    int64_t start;

    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(gpio_num, 1);
    ets_delay_us(1000);
    gpio_set_level(gpio_num, 0);
    ets_delay_us(s_dht.dht_type == 22 ? 1200 : 20000);
    gpio_set_level(gpio_num, 1);
    ets_delay_us(40);
    gpio_set_direction(gpio_num, GPIO_MODE_INPUT);

    start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 0) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }

    for (int i = 0; i < 40; i++) {
        int64_t t0;
        int64_t high_us;
        while (gpio_get_level(gpio_num) == 0) {
        }
        t0 = esp_timer_get_time();
        while (gpio_get_level(gpio_num) == 1) {
        }
        high_us = esp_timer_get_time() - t0;
        data[i / 8] <<= 1;
        if (high_us > 50) {
            data[i / 8] |= 1;
        }
    }

    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4]) {
        return false;
    }

    if (s_dht.dht_type == 22) {
        int16_t raw_t = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
        float hum = ((data[0] << 8) | data[1]) * 0.1f;
        float temp = raw_t * 0.1f;
        if (data[2] & 0x80) {
            temp = -temp;
        }
        s_dht.last_temp_centi = (int16_t)(temp * 100);
        s_dht.last_hum_centi = (uint16_t)(hum * 100);
    } else {
        s_dht.last_temp_centi = (int16_t)(data[2] * 100);
        s_dht.last_hum_centi = (uint16_t)(data[0] * 100);
    }
    return true;
}

esp_err_t drv_dht_get_temperature(int16_t *centi_celsius, void *ctx)
{
    (void)ctx;
    if (centi_celsius == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!drv_dht_sample()) {
        return ESP_FAIL;
    }
    *centi_celsius = s_dht.last_temp_centi;
    return ESP_OK;
}

esp_err_t drv_dht_get_humidity(uint16_t *centi_percent, void *ctx)
{
    (void)ctx;
    if (centi_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* temperature getter already samples both; allow standalone */
    if (!drv_dht_sample()) {
        return ESP_FAIL;
    }
    *centi_percent = s_dht.last_hum_centi;
    return ESP_OK;
}
