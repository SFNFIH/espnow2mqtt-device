/**
 * Stub example: fan — FanControl cluster (mode + percentage).
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ex_fan";
static en2m_fan_mode_t s_mode = EN2M_FAN_OFF;
static uint8_t s_pct = 0;

static esp_err_t stub_set_mode(en2m_fan_mode_t mode, void *ctx)
{
    (void)ctx;
    s_mode = mode;
    if (mode == EN2M_FAN_OFF) {
        s_pct = 0;
    } else if (s_pct == 0) {
        s_pct = 50;
    }
    return ESP_OK;
}
static esp_err_t stub_get_mode(en2m_fan_mode_t *mode, void *ctx)
{
    (void)ctx;
    *mode = s_mode;
    return ESP_OK;
}
static esp_err_t stub_set_pct(uint8_t percent, void *ctx)
{
    (void)ctx;
    s_pct = percent > 100 ? 100 : percent;
    s_mode = (s_pct == 0) ? EN2M_FAN_OFF : EN2M_FAN_ON;
    return ESP_OK;
}
static esp_err_t stub_get_pct(uint8_t *percent, void *ctx)
{
    (void)ctx;
    *percent = s_pct;
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
    en2m_config_t mesh = {.role = EN2M_ROLE_LEAF, .name = "fan1", .model = "ex-fan"};

    ESP_LOGI(TAG, "fan: FanControl stub");
    ESP_ERROR_CHECK(nvs_flash_init());
    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_fan_control(ep, &(en2m_fan_control_driver_t){
                                                          .set_mode = stub_set_mode,
                                                          .get_mode = stub_get_mode,
                                                          .set_percent = stub_set_pct,
                                                          .get_percent = stub_get_pct,
                                                      }));
    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "fan", 6144, NULL, 4, NULL);
}
