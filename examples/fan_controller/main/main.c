/**
 * Fan — FanControl cluster.
 *
 * Uses a per-cluster write callback (en2m_cluster_set_write_cb) instead of the
 * device-wide one. That is the pattern to reach for when a firmware drives
 * several unrelated peripherals: each cluster gets its own handler and its own
 * context, and no handler needs to know about the others.
 */
#include "en2m.h"
#include "esp_log.h"

#define ENDPOINT 1

static const char *TAG = "ex_fan";

typedef struct {
    const char *label;
    uint8_t percent;
    en2m_fan_mode_t mode;
} fan_ctx_t;

static fan_ctx_t s_fan = {.label = "ceiling"};

static void fan_apply(fan_ctx_t *fan)
{
    /* Replace with the LEDC / triac / EC-motor output. */
    ESP_LOGI(TAG, "%s fan: mode=%d duty=%u%%", fan->label, (int)fan->mode, fan->percent);
}

static esp_err_t on_fan_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    fan_ctx_t *fan = (fan_ctx_t *)ctx;

    switch (path->attribute_id) {
    case EN2M_ATTR_FAN_MODE:
        fan->mode = (en2m_fan_mode_t)value->v.e8;
        break;
    case EN2M_ATTR_PERCENT_SETTING:
        fan->percent = value->v.u8;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    fan_apply(fan);
    return ESP_OK;
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "fan1", .model = "ex-fan"},
    };
    en2m_endpoint_t *ep;
    en2m_cluster_t *fan_cluster;

    ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_FAN);
    if (ep == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    fan_cluster = en2m_cluster_get(ep, EN2M_CLUSTER_FAN_CONTROL);
    ESP_ERROR_CHECK(en2m_cluster_set_write_cb(fan_cluster, on_fan_write, &s_fan));

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — mode and percentage are kept consistent by the component");
}
