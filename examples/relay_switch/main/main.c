/**
 * Relay switch — OnOff cluster.
 *
 * Shows the two directions of control with no application loop at all:
 *  - remote:  the component calls on_write() and reports once the relay moved
 *  - local:   the button ISR defers a toggle onto the en2m task
 */
#include "en2m.h"
#include "esp_log.h"

#include "../../../drivers/drv_gpio_button.h"
#include "../../../drivers/drv_gpio_relay.h"

#define PIN_RELAY GPIO_NUM_5
#define PIN_BUTTON GPIO_NUM_9
#define ENDPOINT 1

static const char *TAG = "ex_switch";

/** The only place this firmware touches the relay. */
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    if (path->cluster_id == EN2M_CLUSTER_ON_OFF) {
        return drv_gpio_relay_set(value->v.b, ctx);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static void on_attribute_changed(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)path;
    (void)ctx;
    ESP_LOGI(TAG, "relay is now %s", value->v.b ? "on" : "off");
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
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"},
        .attribute_write = on_write,
        .attribute_changed = on_attribute_changed,
    };

    ESP_ERROR_CHECK(drv_gpio_relay_init(PIN_RELAY, true));
    ESP_ERROR_CHECK(drv_gpio_button_init(PIN_BUTTON, true, 40, on_button, NULL));

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    /* The relay state is persisted, so en2m_start restores it into the driver. */
    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — the component owns the task from here");
}
