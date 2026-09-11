/**
 * Stub example: dimmable CCT light — OnOff + Level + ColorControl (mireds).
 * Replace stub drivers with PWM / LED IC drivers.
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ex_light";
static bool s_on = true;
static uint8_t s_level = 200;
static uint16_t s_mireds = 300;

static esp_err_t stub_set_on(bool on, void *ctx)
{
    (void)ctx;
    s_on = on;
    return ESP_OK;
}
static esp_err_t stub_get_on(bool *on, void *ctx)
{
    (void)ctx;
    *on = s_on;
    return ESP_OK;
}
static esp_err_t stub_set_level(uint8_t level, void *ctx)
{
    (void)ctx;
    s_level = level;
    if (level > 0) {
        s_on = true;
    }
    return ESP_OK;
}
static esp_err_t stub_get_level(uint8_t *level, void *ctx)
{
    (void)ctx;
    *level = s_level;
    return ESP_OK;
}
static esp_err_t stub_set_ct(uint16_t mireds, void *ctx)
{
    (void)ctx;
    s_mireds = mireds;
    return ESP_OK;
}
static esp_err_t stub_get_ct(uint16_t *mireds, void *ctx)
{
    (void)ctx;
    *mireds = s_mireds;
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
    en2m_config_t mesh = {
        .role = EN2M_ROLE_LEAF,
        .name = "light1",
        .model = "ex-light",
    };

    ESP_LOGI(TAG, "light: OnOff+Level+ColorTemp stubs");
    ESP_ERROR_CHECK(nvs_flash_init());
    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_on_off(ep, &(en2m_on_off_driver_t){.set = stub_set_on, .get = stub_get_on}));
    ESP_ERROR_CHECK(en2m_endpoint_add_level_control(ep, &(en2m_level_driver_t){.set_level = stub_set_level,
                                                                                .get_level = stub_get_level}));
    ESP_ERROR_CHECK(en2m_endpoint_add_color_control(ep, &(en2m_color_control_driver_t){.set_color_temp = stub_set_ct,
                                                                                        .get_color_temp = stub_get_ct}));
    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "light", 6144, NULL, 4, NULL);
}
