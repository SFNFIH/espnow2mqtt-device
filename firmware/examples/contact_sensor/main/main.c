/**
 * Example: contact / door — Boolean State cluster + GPIO contact driver.
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "../../../drivers/drv_gpio_contact.h"

static const char *TAG = "ex_contact";
static bool s_last_open;

static void app_task(void *arg)
{
    (void)arg;
    drv_gpio_contact_get(&s_last_open, NULL);
    while (1) {
        bool open = false;
        en2m_model_loop();
        if (drv_gpio_contact_get(&open, NULL) == ESP_OK && open != s_last_open) {
            s_last_open = open;
            en2m_model_notify(1, EN2M_CLUSTER_BOOLEAN_STATE, true);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_config_t mesh = {
        .role = EN2M_ROLE_LEAF,
        .name = "door1",
        .model = "ex-contact",
    };

    ESP_LOGI(TAG, "contact: BooleanState ← GPIO driver");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(drv_gpio_contact_init(GPIO_NUM_9, false));

    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_boolean_state(ep, &(en2m_boolean_state_driver_t){
                                                            .get = drv_gpio_contact_get,
                                                        }));

    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "contact", 6144, NULL, 4, NULL);
}
