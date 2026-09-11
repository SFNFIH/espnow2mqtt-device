/**
 * Occupancy + illuminance sensor.
 *
 * Occupancy is event driven: a PIR would raise an interrupt, and here an
 * esp_timer stands in for it. Either way the handler only defers work onto
 * the en2m task with en2m_schedule, so no application task exists.
 */
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#define ENDPOINT 1
#define SIMULATED_MOTION_PERIOD_US (15 * 1000 * 1000)

static const char *TAG = "ex_occ";

static bool s_occupied;

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_ILLUMINANCE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* A real driver would read BH1750 / TSL2591 here. */
    *out_value = en2m_u32(s_occupied ? 480 : 80);
    return ESP_OK;
}

static void publish_motion(void *arg)
{
    (void)arg;
    en2m_report_occupancy(ENDPOINT, s_occupied);
}

/** Stands in for the PIR interrupt. */
static void simulate_motion(void *arg)
{
    (void)arg;
    s_occupied = !s_occupied;
    en2m_schedule(publish_motion, NULL);
}

static esp_err_t start_motion_simulation(void)
{
    const esp_timer_create_args_t args = {
        .callback = simulate_motion,
        .name = "pir_stub",
    };
    esp_timer_handle_t timer;

    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "timer create failed");
    return esp_timer_start_periodic(timer, SIMULATED_MOTION_PERIOD_US);
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "occ1", .model = "ex-occ"},
        .attribute_read = on_read,
    };

    ep = en2m_endpoint_create(ENDPOINT);
    if (ep == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_OCCUPANCY_SENSOR));
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_LIGHT_SENSOR));

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_ERROR_CHECK(start_motion_simulation());
    ESP_LOGI(TAG, "ready — motion is pushed, illuminance is pulled");
}
