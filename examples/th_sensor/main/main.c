/**
 * Temperature / humidity sensor — pull-style sensor, zero application code
 * outside the read callback.
 *
 * The component calls on_read() for each attribute right before it builds a
 * report, so the DHT is sampled exactly as often as the mesh needs it.
 */
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"

#include "../../../drivers/drv_dht.h"

#define PIN_DHT GPIO_NUM_4
#define DHT_TYPE 22
#define ENDPOINT 1

static const char *TAG = "ex_th";

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    switch (path->cluster_id) {
    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT: {
        int16_t centi_celsius = 0;
        ESP_RETURN_ON_ERROR(drv_dht_get_temperature(&centi_celsius, ctx), TAG, "temperature read failed");
        *out_value = en2m_i16(centi_celsius);
        return ESP_OK;
    }
    case EN2M_CLUSTER_RELATIVE_HUMIDITY: {
        uint16_t centi_percent = 0;
        ESP_RETURN_ON_ERROR(drv_dht_get_humidity(&centi_percent, ctx), TAG, "humidity read failed");
        *out_value = en2m_u16(centi_percent);
        return ESP_OK;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "th_sensor1", .model = "ex-th"},
        .attribute_read = on_read,
        /* A DHT22 cannot be sampled faster than every two seconds. */
        .report_interval_ms = 60000,
        .min_report_interval_ms = 5000,
    };

    ESP_ERROR_CHECK(drv_dht_init(PIN_DHT, DHT_TYPE));

    ep = en2m_endpoint_create(ENDPOINT);
    if (ep == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR));
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_HUMIDITY_SENSOR));

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — sampling happens on demand, not in a loop");
}
