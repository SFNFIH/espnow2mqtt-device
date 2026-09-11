/**
 * Example: relay switch — OnOff cluster + GPIO relay driver.
 * Local button is also driver-side input that notifies the model.
 */
#include "driver/gpio.h"
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "../../../drivers/drv_gpio_relay.h"

#ifndef PIN_BUTTON
#define PIN_BUTTON GPIO_NUM_9
#endif

static const char *TAG = "ex_switch";

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
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_config_t mesh = {
        .role = EN2M_ROLE_LEAF,
        .name = "relay1",
        .model = "ex-switch",
    };

    ESP_LOGI(TAG, "switch: OnOff ← relay driver");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(drv_gpio_relay_init(GPIO_NUM_5, true));

    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_on_off(ep, &(en2m_on_off_driver_t){
                                                     .set = drv_gpio_relay_set,
                                                     .get = drv_gpio_relay_get,
                                                 }));

    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "sw", 6144, NULL, 4, NULL);
}
