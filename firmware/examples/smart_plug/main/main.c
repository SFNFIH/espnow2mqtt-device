/**
 * Example: smart plug — OnOff + Electrical Power clusters.
 * Power values come from a stub driver (replace with real metering IC).
 */
#include "driver/gpio.h"
#include "en2m.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "../../../drivers/drv_gpio_relay.h"

#ifndef PIN_BUTTON
#define PIN_BUTTON GPIO_NUM_9
#endif

static const char *TAG = "ex_plug";
static int32_t s_power_mw;
static int64_t s_energy_mwh;

static esp_err_t stub_get_power(int32_t *milliwatts, void *ctx)
{
    bool on = false;
    (void)ctx;
    drv_gpio_relay_get(&on, NULL);
    if (!on) {
        s_power_mw = 0;
    } else {
        s_power_mw = (int32_t)((20 * 1000) + (esp_random() % 40000));
    }
    *milliwatts = s_power_mw;
    return ESP_OK;
}

static esp_err_t stub_get_energy(int64_t *milliwatt_hours, void *ctx)
{
    (void)ctx;
    /* accumulate roughly from last power sample */
    s_energy_mwh += s_power_mw / 240; /* crude ~15s tick integration helper */
    *milliwatt_hours = s_energy_mwh;
    return ESP_OK;
}

static void app_task(void *arg)
{
    int last_btn = 1;
    (void)arg;
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    gpio_config(&in);

    while (1) {
        int btn = gpio_get_level(PIN_BUTTON);
        en2m_model_loop();
        if (last_btn == 1 && btn == 0) {
            bool on = false;
            drv_gpio_relay_get(&on, NULL);
            drv_gpio_relay_set(!on, NULL);
            en2m_model_notify(1, EN2M_CLUSTER_ON_OFF, true);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        last_btn = btn;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_config_t mesh = {
        .role = EN2M_ROLE_LEAF,
        .name = "plug1",
        .model = "ex-plug",
    };

    ESP_LOGI(TAG, "plug: OnOff+ElectricalPower ← drivers");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(drv_gpio_relay_init(GPIO_NUM_5, true));

    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_on_off(ep, &(en2m_on_off_driver_t){
                                                     .set = drv_gpio_relay_set,
                                                     .get = drv_gpio_relay_get,
                                                 }));
    ESP_ERROR_CHECK(en2m_endpoint_add_electrical_power(ep, &(en2m_electrical_power_driver_t){
                                                               .get_active_power = stub_get_power,
                                                               .get_energy = stub_get_energy,
                                                           }));

    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "plug", 6144, NULL, 4, NULL);
}
