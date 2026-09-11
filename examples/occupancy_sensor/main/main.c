/**
 * Stub example: occupancy + illuminance (+ optional smoke).
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ex_occ";
static bool s_occ = false;
static uint32_t s_lux = 120;
static bool s_smoke = false;

static esp_err_t stub_occ(bool *occupied, void *ctx)
{
    (void)ctx;
    *occupied = s_occ;
    return ESP_OK;
}
static esp_err_t stub_lux(uint32_t *lux, void *ctx)
{
    (void)ctx;
    *lux = s_lux;
    return ESP_OK;
}
static esp_err_t stub_smoke(bool *alarm, void *ctx)
{
    (void)ctx;
    *alarm = s_smoke;
    return ESP_OK;
}

static void app_task(void *arg)
{
    int tick = 0;
    (void)arg;
    while (1) {
        en2m_model_loop();
        /* Simulate motion every ~15s for demo */
        if (++tick >= 300) {
            tick = 0;
            s_occ = !s_occ;
            s_lux = 80 + (s_occ ? 400 : 0);
            en2m_model_notify(1, EN2M_CLUSTER_OCCUPANCY, true);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_config_t mesh = {.role = EN2M_ROLE_LEAF, .name = "occ1", .model = "ex-occ"};

    ESP_LOGI(TAG, "occupancy + illuminance + smoke stubs");
    ESP_ERROR_CHECK(nvs_flash_init());
    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_occupancy(ep, &(en2m_occupancy_driver_t){.get_occupied = stub_occ}));
    ESP_ERROR_CHECK(en2m_endpoint_add_illuminance(ep, &(en2m_illuminance_driver_t){.get_lux = stub_lux}));
    ESP_ERROR_CHECK(en2m_endpoint_add_smoke_co(ep, &(en2m_smoke_co_driver_t){.get_smoke = stub_smoke}));
    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "occ", 6144, NULL, 4, NULL);
}
