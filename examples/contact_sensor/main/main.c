/**
 * Contact / door sensor — Boolean State cluster.
 *
 * Edge driven: the GPIO interrupt defers a read onto the en2m task, which
 * publishes the new value and reports it immediately. The read callback is a
 * cheap safety net that resynchronizes the state on every periodic report.
 */
#include "en2m.h"
#include "esp_log.h"

#include "../../../drivers/drv_gpio_contact.h"

#define PIN_CONTACT GPIO_NUM_9
#define CONTACT_ACTIVE_LOW true
#define ENDPOINT 1

static const char *TAG = "ex_contact";

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    bool open = false;

    if (path->cluster_id != EN2M_CLUSTER_BOOLEAN_STATE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (drv_gpio_contact_get(&open, ctx) != ESP_OK) {
        return ESP_FAIL;
    }
    *out_value = en2m_bool(open);
    return ESP_OK;
}

static void publish_contact(void *arg)
{
    bool open = false;

    (void)arg;
    if (drv_gpio_contact_get(&open, NULL) == ESP_OK) {
        en2m_report_boolean_state(ENDPOINT, open);
    }
}

static void on_contact_edge(void *arg)
{
    BaseType_t woken = pdFALSE;

    (void)arg;
    en2m_schedule_from_isr(publish_contact, NULL, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "door1", .model = "ex-contact"},
        .attribute_read = on_read,
    };

    ESP_ERROR_CHECK(drv_gpio_contact_init(PIN_CONTACT, CONTACT_ACTIVE_LOW));

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_CONTACT_SENSOR) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(drv_gpio_contact_watch(on_contact_edge, NULL));
    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — state changes travel on the interrupt, not on a poll");
}
