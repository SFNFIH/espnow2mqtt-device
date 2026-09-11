/**
 * Pure mesh router — interaction layer not required; transport only.
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "router";

static void app_task(void *arg)
{
    (void)arg;
    while (1) {
        en2m_mesh_loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    en2m_config_t cfg = {
        .role = EN2M_ROLE_ROUTER,
        .name = "router1",
        .model = "ex-router",
    };
    ESP_LOGI(TAG, "router: transport only (no device clusters)");
    ESP_ERROR_CHECK(en2m_mesh_init(&cfg));
    xTaskCreate(app_task, "router", 4096, NULL, 4, NULL);
}
