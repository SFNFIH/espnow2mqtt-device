/**
 * Relay switch — OnOff cluster.
 *
 * Shows the two directions of control with no application loop at all:
 *  - remote:  the component calls on_write() and reports once the relay moved
 *  - local:   the button callback defers a toggle onto the en2m task
 *
 * The button is `espressif/button`, which debounces and classifies presses on
 * its own esp_timer. That timer is shared by every button in the firmware, so
 * its callbacks must stay short — hence ::en2m_schedule rather than doing the
 * work in place.
 *
 * The relay is a plain GPIO output. There is no relay component in the registry
 * because there is nothing to abstract: one pin, one level.
 */
#include "button_gpio.h"
#include "driver/gpio.h"
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "iot_button.h"

#define PIN_RELAY GPIO_NUM_5
#define PIN_BUTTON GPIO_NUM_9
#define RELAY_ACTIVE_HIGH true
#define BUTTON_ACTIVE_LEVEL 0
#define ENDPOINT 1

static const char *TAG = "ex_switch";

/** The only place this firmware touches the relay. */
static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_ON_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return gpio_set_level(PIN_RELAY, value->v.b == RELAY_ACTIVE_HIGH);
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
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "relay1", .model = "ex-switch"},
        .attribute_write = on_write,
        .attribute_changed = on_attribute_changed,
    };

    ESP_ERROR_CHECK(relay_init());
    ESP_ERROR_CHECK(button_init());

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_ON_OFF_PLUG) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    /* The relay state is persisted, so en2m_start restores it through on_write. */
    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — the component owns the task from here");
}
