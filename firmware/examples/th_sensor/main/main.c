/**
 * Example: temperature / humidity — interaction via en2m clusters,
 * DHT hardware only in drv_dht (driver layer).
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "../../../drivers/drv_dht.h"

static const char *TAG = "ex_th";

static void app_task(void *arg)
{
    (void)arg;
    while (1) {
        en2m_model_loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_config_t mesh = {
        .role = EN2M_ROLE_LEAF,
        .name = "th_sensor1",
        .model = "ex-th",
    };

    ESP_LOGI(TAG, "TH sensor: model binds DHT driver → clusters");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(drv_dht_init(GPIO_NUM_4, 22));

    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_temperature(ep, &(en2m_temperature_driver_t){
                                                          .get_measured_value = drv_dht_get_temperature,
                                                      }));
    ESP_ERROR_CHECK(en2m_endpoint_add_humidity(ep, &(en2m_humidity_driver_t){
                                                       .get_measured_value = drv_dht_get_humidity,
                                                   }));

    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "th", 6144, NULL, 4, NULL);
}
