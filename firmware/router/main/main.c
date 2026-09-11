/**
 * Pure mesh ROUTER — always-powered forwarder, no local sensors/actuators.
 * Place between coordinator and far leaves.
 */
#include "en2m_example_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "router";

static void app_task(void *arg)
{
    (void)arg;
    char json[EN2M_DATA_MAX];
    en2m_example_build_base_json(json, sizeof(json), "router", NULL);
    en2m_example_send_hello(json);
    while (1) {
        en2m_example_mesh_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "mesh router (forward-only)");
    en2m_example_cfg_t cfg = {
        .role = EN2M_ROLE_ROUTER,
        .name = "router1",
        .model = "ex-router",
        .fw = "0.3.0-idf",
        .on_command = NULL,
    };
    ESP_ERROR_CHECK(en2m_example_mesh_start(&cfg));
    xTaskCreate(app_task, "router", 4096, NULL, 4, NULL);
}
