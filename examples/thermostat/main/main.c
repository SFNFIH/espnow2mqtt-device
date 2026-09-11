/**
 * Stub example: thermostat — Thermostat cluster (+ local temp).
 */
#include "en2m.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ex_climate";
static en2m_thermostat_mode_t s_mode = EN2M_THERMOSTAT_HEAT;
static int16_t s_heat = 2100;
static int16_t s_cool = 2400;
static int16_t s_local = 2200;

static esp_err_t stub_set_mode(en2m_thermostat_mode_t mode, void *ctx)
{
    (void)ctx;
    s_mode = mode;
    return ESP_OK;
}
static esp_err_t stub_get_mode(en2m_thermostat_mode_t *mode, void *ctx)
{
    (void)ctx;
    *mode = s_mode;
    return ESP_OK;
}
static esp_err_t stub_set_heat(int16_t c, void *ctx)
{
    (void)ctx;
    s_heat = c;
    return ESP_OK;
}
static esp_err_t stub_get_heat(int16_t *c, void *ctx)
{
    (void)ctx;
    *c = s_heat;
    return ESP_OK;
}
static esp_err_t stub_set_cool(int16_t c, void *ctx)
{
    (void)ctx;
    s_cool = c;
    return ESP_OK;
}
static esp_err_t stub_get_cool(int16_t *c, void *ctx)
{
    (void)ctx;
    *c = s_cool;
    return ESP_OK;
}
static esp_err_t stub_get_local(int16_t *c, void *ctx)
{
    (void)ctx;
    *c = s_local;
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
    en2m_config_t mesh = {.role = EN2M_ROLE_LEAF, .name = "thermo1", .model = "ex-climate"};

    ESP_LOGI(TAG, "climate: Thermostat stub");
    ESP_ERROR_CHECK(nvs_flash_init());
    ep = en2m_endpoint_create(1);
    ESP_ERROR_CHECK(en2m_endpoint_add_thermostat(ep, &(en2m_thermostat_driver_t){
                                                         .set_system_mode = stub_set_mode,
                                                         .get_system_mode = stub_get_mode,
                                                         .set_occupied_heating = stub_set_heat,
                                                         .get_occupied_heating = stub_get_heat,
                                                         .set_occupied_cooling = stub_set_cool,
                                                         .get_occupied_cooling = stub_get_cool,
                                                         .get_local_temperature = stub_get_local,
                                                     }));
    ESP_ERROR_CHECK(en2m_model_start(&mesh));
    xTaskCreate(app_task, "climate", 6144, NULL, 4, NULL);
}
