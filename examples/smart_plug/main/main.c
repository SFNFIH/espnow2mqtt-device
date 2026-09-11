/**
 * Smart plug — OnOff + Electrical Power.
 *
 * Mixes both callback directions: the relay is driven by writes, while the
 * metering values are pulled through the read callback whenever a report is
 * due. Replace the stub with a real metering IC (BL0937, HLW8012, …).
 */
#include "en2m.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "../../../drivers/drv_gpio_button.h"
#include "../../../drivers/drv_gpio_relay.h"

#define PIN_RELAY GPIO_NUM_5
#define PIN_BUTTON GPIO_NUM_9
#define ENDPOINT 1

static const char *TAG = "ex_plug";

static struct {
    int32_t power_mw;
    int64_t energy_mwh;
    int64_t last_sample_us;
} s_meter;

/** Integrate the previous sample into the energy counter, then take a new one. */
static int32_t meter_sample(void)
{
    int64_t now = esp_timer_get_time();
    bool on = false;

    if (s_meter.last_sample_us != 0) {
        int64_t elapsed_us = now - s_meter.last_sample_us;
        s_meter.energy_mwh += (int64_t)s_meter.power_mw * elapsed_us / (3600LL * 1000000LL);
    }
    s_meter.last_sample_us = now;

    drv_gpio_relay_get(&on, NULL);
    s_meter.power_mw = on ? (int32_t)(20000 + (esp_random() % 40000)) : 0;
    return s_meter.power_mw;
}

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
        return drv_gpio_relay_set(value->v.b, ctx);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_ELECTRICAL_POWER) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (path->attribute_id == EN2M_ATTR_ACTIVE_POWER_MW) {
        *out_value = en2m_i32(meter_sample());
        return ESP_OK;
    }
    if (path->attribute_id == EN2M_ATTR_ENERGY_MWH) {
        *out_value = en2m_i64(s_meter.energy_mwh);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static void toggle(void *arg)
{
    en2m_value_t current;

    (void)arg;
    if (en2m_attribute_get(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, &current) != ESP_OK) {
        return;
    }
    en2m_attribute_write(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!current.v.b));
}

static void on_button(void *ctx)
{
    BaseType_t woken = pdFALSE;

    (void)ctx;
    en2m_schedule_from_isr(toggle, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "plug1", .model = "ex-plug"},
        .attribute_write = on_write,
        .attribute_read = on_read,
        .report_interval_ms = 15000,
    };

    ESP_ERROR_CHECK(drv_gpio_relay_init(PIN_RELAY, true));
    ESP_ERROR_CHECK(drv_gpio_button_init(PIN_BUTTON, true, 40, on_button, NULL));

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_SMART_PLUG) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — metering is sampled on demand");
}
