/**
 * Smart plug — OnOff + Electrical Power.
 *
 * Mixes both callback directions: the relay is driven by writes, while the
 * metering values are pulled through the read callback whenever a report is
 * due. Replace the stub with a real metering IC (BL0937, HLW8012, …).
 *
 * The button is `espressif/button`; the relay is a plain GPIO output, because
 * one pin at one level is not worth a driver.
 */
#include "button_gpio.h"
#include "driver/gpio.h"
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "iot_button.h"

#define PIN_RELAY GPIO_NUM_5
#define PIN_BUTTON GPIO_NUM_9
#define RELAY_ACTIVE_HIGH true
#define BUTTON_ACTIVE_LEVEL 0
#define ENDPOINT 1

static const char *TAG = "ex_plug";

static struct {
    int32_t power_mw;
    int64_t energy_mwh;
    int64_t last_sample_us;
} s_meter;

/* Mirrors the relay pin so the metering stub does not have to read an
 * attribute back while the component already holds its lock. */
static bool s_relay_on;

/** Integrate the previous sample into the energy counter, then take a new one. */
static int32_t meter_sample(void)
{
    int64_t now = esp_timer_get_time();

    if (s_meter.last_sample_us != 0) {
        int64_t elapsed_us = now - s_meter.last_sample_us;
        s_meter.energy_mwh += (int64_t)s_meter.power_mw * elapsed_us / (3600LL * 1000000LL);
    }
    s_meter.last_sample_us = now;

    s_meter.power_mw = s_relay_on ? (int32_t)(20000 + (esp_random() % 40000)) : 0;
    return s_meter.power_mw;
}

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_ON_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_relay_on = value->v.b;
    return gpio_set_level(PIN_RELAY, s_relay_on == RELAY_ACTIVE_HIGH);
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

/** Runs on the en2m task, so it may use the full API. */
static void toggle(void *arg)
{
    en2m_value_t current;

    (void)arg;
    if (en2m_attribute_get(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, &current) != ESP_OK) {
        return;
    }
    en2m_attribute_write(ENDPOINT, EN2M_CLUSTER_ON_OFF, EN2M_ATTR_ON_OFF, en2m_bool(!current.v.b));
}

static void on_button(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    en2m_schedule(toggle, NULL);
}

static esp_err_t relay_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_RELAY,
        .mode = GPIO_MODE_OUTPUT,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "relay gpio config failed");
    return gpio_set_level(PIN_RELAY, !RELAY_ACTIVE_HIGH);
}

static esp_err_t button_init(void)
{
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = PIN_BUTTON,
        .active_level = BUTTON_ACTIVE_LEVEL,
    };
    button_handle_t btn = NULL;

    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn), TAG,
                        "button init failed");
    return iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_button, NULL);
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "plug1", .model = "ex-plug"},
        .attribute_write = on_write,
        .attribute_read = on_read,
        .report_interval_ms = 15000,
    };

    ESP_ERROR_CHECK(relay_init());
    ESP_ERROR_CHECK(button_init());

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_SMART_PLUG) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — metering is sampled on demand");
}
