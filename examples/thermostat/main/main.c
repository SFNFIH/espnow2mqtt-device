/**
 * Thermostat — Thermostat cluster.
 *
 * Writes carry the mode and the setpoints, the read callback supplies the
 * measured room temperature, and the changed callback runs the local control
 * loop. Nothing here needs a task: every decision is triggered by an event.
 */
#include "en2m.h"
#include "esp_log.h"

#define ENDPOINT 1

static const char *TAG = "ex_climate";

static struct {
    en2m_thermostat_mode_t mode;
    int16_t heating_centi;
    int16_t cooling_centi;
    int16_t local_centi;
    bool relay_on;
} s_hvac = {.mode = EN2M_THERMOSTAT_HEAT, .heating_centi = 2100, .cooling_centi = 2400,
            .local_centi = 2200};

/** Decide whether the heat call should be active for the current state. */
static void hvac_control(void)
{
    bool want = s_hvac.relay_on;

    switch (s_hvac.mode) {
    case EN2M_THERMOSTAT_HEAT:
        want = s_hvac.local_centi < s_hvac.heating_centi - 20;
        if (s_hvac.local_centi > s_hvac.heating_centi + 20) {
            want = false;
        }
        break;
    case EN2M_THERMOSTAT_COOL:
        want = s_hvac.local_centi > s_hvac.cooling_centi + 20;
        if (s_hvac.local_centi < s_hvac.cooling_centi - 20) {
            want = false;
        }
        break;
    case EN2M_THERMOSTAT_OFF:
        want = false;
        break;
    default:
        break;
    }

    if (want != s_hvac.relay_on) {
        s_hvac.relay_on = want;
        ESP_LOGI(TAG, "demand relay %s", want ? "on" : "off");
    }
}

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_THERMOSTAT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    switch (path->attribute_id) {
    case EN2M_ATTR_SYSTEM_MODE:
        s_hvac.mode = (en2m_thermostat_mode_t)value->v.e8;
        break;
    case EN2M_ATTR_OCCUPIED_HEATING_SETPOINT:
        s_hvac.heating_centi = value->v.i16;
        break;
    case EN2M_ATTR_OCCUPIED_COOLING_SETPOINT:
        s_hvac.cooling_centi = value->v.i16;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    hvac_control();
    return ESP_OK;
}

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_THERMOSTAT || path->attribute_id != EN2M_ATTR_LOCAL_TEMPERATURE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Replace with the real room sensor; the stub drifts toward the setpoint. */
    if (s_hvac.relay_on) {
        s_hvac.local_centi += 5;
    } else {
        s_hvac.local_centi -= 2;
    }
    *out_value = en2m_i16(s_hvac.local_centi);
    return ESP_OK;
}

static void on_attribute_changed(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)ctx;

    if (path->attribute_id == EN2M_ATTR_LOCAL_TEMPERATURE) {
        s_hvac.local_centi = value->v.i16;
        hvac_control();
    }
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "thermo1", .model = "ex-climate"},
        .attribute_write = on_write,
        .attribute_read = on_read,
        .attribute_changed = on_attribute_changed,
    };

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_THERMOSTAT) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — setpoints survive a reboot");
}
