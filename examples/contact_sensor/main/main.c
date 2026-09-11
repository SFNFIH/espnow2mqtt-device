/**
 * Contact / door sensor — Boolean State cluster.
 *
 * A reed switch is electrically a button, so this uses `espressif/button`
 * rather than a bespoke GPIO driver: registering both BUTTON_PRESS_DOWN and
 * BUTTON_PRESS_UP gives a debounced callback on each edge.
 *
 * Event driven: the edge callback defers a read onto the en2m task, which
 * publishes the new value and reports it immediately. The read callback is a
 * cheap safety net that resynchronizes the state on every periodic report.
 */
#include "button_gpio.h"
#include "driver/gpio.h"
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "iot_button.h"

#define PIN_CONTACT GPIO_NUM_9
#define CONTACT_ACTIVE_LEVEL 0
/** true when the switch conducting means "door open". Flip for the other wiring. */
#define CONTACT_OPEN_WHEN_ACTIVE true
#define ENDPOINT 1

static const char *TAG = "ex_contact";

static button_handle_t s_contact;

/** Reads the pin straight through; no debounce needed for a level query. */
static bool contact_is_open(void)
{
    return (iot_button_get_key_level(s_contact) == BUTTON_ACTIVE) == CONTACT_OPEN_WHEN_ACTIVE;
}

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_BOOLEAN_STATE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *out_value = en2m_bool(contact_is_open());
    return ESP_OK;
}

static void publish_contact(void *arg)
{
    (void)arg;
    en2m_report_boolean_state(ENDPOINT, contact_is_open());
}

static void on_contact_edge(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    en2m_schedule(publish_contact, NULL);
}

static esp_err_t contact_init(void)
{
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = PIN_CONTACT,
        .active_level = CONTACT_ACTIVE_LEVEL,
    };

    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_contact), TAG,
                        "contact init failed");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(s_contact, BUTTON_PRESS_DOWN, NULL, on_contact_edge, NULL),
                        TAG, "cannot watch the closing edge");
    return iot_button_register_cb(s_contact, BUTTON_PRESS_UP, NULL, on_contact_edge, NULL);
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "door1", .model = "ex-contact"},
        .attribute_read = on_read,
    };

    ESP_ERROR_CHECK(contact_init());

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_CONTACT_SENSOR) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — state changes travel on the edge callback, not on a poll");
}
