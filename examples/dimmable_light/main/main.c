/**
 * Tunable white light — OnOff + Level + ColorControl.
 *
 * One write callback covers all three clusters. Replace the stub with LEDC,
 * PWM or an LED driver IC; the interaction layer does not change.
 */
#include "en2m.h"
#include "esp_log.h"

#define ENDPOINT 1

static const char *TAG = "ex_light";

/* Stand-in for the real light engine. */
static struct {
    bool on;
    uint8_t level;
    uint16_t mireds;
} s_light = {.on = false, .level = 254, .mireds = 300};

static void light_apply(void)
{
    ESP_LOGI(TAG, "output: %s level=%u mireds=%u", s_light.on ? "on" : "off", s_light.level,
             s_light.mireds);
}

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)ctx;

    switch (path->cluster_id) {
    case EN2M_CLUSTER_ON_OFF:
        s_light.on = value->v.b;
        break;
    case EN2M_CLUSTER_LEVEL_CONTROL:
        s_light.level = value->v.u8;
        break;
    case EN2M_CLUSTER_COLOR_CONTROL:
        s_light.mireds = value->v.u16;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    light_apply();
    return ESP_OK;
}

static void on_identify(uint8_t endpoint_id, uint16_t seconds, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "identify endpoint %u, %u s left", endpoint_id, seconds);
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "light1", .model = "ex-light"},
        .attribute_write = on_write,
        .identify = on_identify,
    };
    en2m_endpoint_t *ep;

    ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT);
    if (ep == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }
    en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — brightness and colour temperature arrive as writes");
}
