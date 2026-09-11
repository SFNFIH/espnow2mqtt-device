/**
 * Stub example: door lock — DoorLock cluster.
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ex_lock";
static bool s_locked = true;

static esp_err_t stub_lock(void *ctx)
{
    (void)ctx;
    s_locked = true;
    return ESP_OK;
}
static esp_err_t stub_unlock(void *ctx)
{
    (void)ctx;
    s_locked = false;
    return ESP_OK;
}
static esp_err_t stub_get(bool *locked, void *ctx)
{
    (void)ctx;
    *locked = s_locked;
    return ESP_OK;
}

static void app_task(void *arg)
{
    (void)arg;
    while (1) {
        en2m_model_loop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_config_t mesh = {.role = EN2M_ROLE_LEAF, .name = "lock1", .model = "ex-lock"};

    ESP_LOGI(TAG, "lock: DoorLock stub");
    ESP_ERROR_CHECK(nvs_flash_init());
    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_door_lock(ep, &(en2m_door_lock_driver_t){
                                                        .lock = stub_lock,
                                                        .unlock = stub_unlock,
                                                        .get_locked = stub_get,
                                                    }));
    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "lock", 6144, NULL, 4, NULL);
}
