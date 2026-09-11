/**
 * Stub example: window covering — WindowCovering cluster.
 * position: 0 = open, 100 = closed.
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ex_cover";
static uint8_t s_pos = 0;

static esp_err_t stub_cmd(en2m_cover_command_t cmd, uint8_t position, void *ctx)
{
    (void)ctx;
    switch (cmd) {
    case EN2M_COVER_OPEN:
        s_pos = 0;
        break;
    case EN2M_COVER_CLOSE:
        s_pos = 100;
        break;
    case EN2M_COVER_GOTO:
        s_pos = position > 100 ? 100 : position;
        break;
    case EN2M_COVER_STOP:
    default:
        break;
    }
    return ESP_OK;
}
static esp_err_t stub_get(uint8_t *position, void *ctx)
{
    (void)ctx;
    *position = s_pos;
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
    en2m_config_t mesh = {.role = EN2M_ROLE_LEAF, .name = "cover1", .model = "ex-cover"};

    ESP_LOGI(TAG, "cover: WindowCovering stub");
    ESP_ERROR_CHECK(nvs_flash_init());
    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_window_covering(ep, &(en2m_window_covering_driver_t){
                                                              .command = stub_cmd,
                                                              .get_position = stub_get,
                                                          }));
    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "cover", 6144, NULL, 4, NULL);
}
