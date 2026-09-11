/**
 * Occupancy + illuminance sensor — one endpoint, two device types, both
 * callback directions.
 *
 * Occupancy is pushed: a PIR module is an open-drain output that idles at one
 * level and asserts while it sees motion, which is exactly what a button is,
 * so `espressif/button` drives it. PRESS_DOWN means motion started and
 * PRESS_UP means the module's own hold time expired.
 *
 * Illuminance is pulled: the BH1750 runs in continuous mode, so the read
 * callback only has to fetch the last conversion — no waiting inside the
 * en2m task. The occupancy read callback is a cheap safety net that
 * resynchronizes the state on every periodic report in case an edge was lost.
 */
#include "bh1750.h"
#include "button_gpio.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "iot_button.h"

#define PIN_SDA 4
#define PIN_SCL 5
#define I2C_PORT I2C_NUM_0
#define PIN_PIR GPIO_NUM_6
/** A PIR output idles low and goes high while it detects motion. */
#define PIR_ACTIVE_LEVEL 1
#define ENDPOINT 1

static const char *TAG = "ex_occ";

static bh1750_handle_t s_bh1750;
static button_handle_t s_pir;

static bool pir_is_active(void)
{
    return iot_button_get_key_level(s_pir) == BUTTON_ACTIVE;
}

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    (void)ctx;

    switch (path->cluster_id) {
    case EN2M_CLUSTER_ILLUMINANCE: {
        float lux = 0.0f;
        ESP_RETURN_ON_ERROR(bh1750_get_data(s_bh1750, &lux), TAG, "bh1750 read failed");
        *out_value = en2m_u32((uint32_t)(lux < 0.0f ? 0.0f : lux));
        return ESP_OK;
    }
    case EN2M_CLUSTER_OCCUPANCY:
        *out_value = en2m_bool(pir_is_active());
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

/** Runs on the en2m task, so it may use the full API. */
static void publish_motion(void *arg)
{
    (void)arg;
    en2m_report_occupancy(ENDPOINT, pir_is_active());
}

static void on_pir_edge(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    en2m_schedule(publish_motion, NULL);
}

static esp_err_t pir_init(void)
{
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = PIN_PIR,
        .active_level = PIR_ACTIVE_LEVEL,
        /* The PIR drives the line itself; an internal pull would fight it. */
        .disable_pull = true,
    };

    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_pir), TAG,
                        "pir init failed");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(s_pir, BUTTON_PRESS_DOWN, NULL, on_pir_edge, NULL),
                        TAG, "cannot watch motion start");
    return iot_button_register_cb(s_pir, BUTTON_PRESS_UP, NULL, on_pir_edge, NULL);
}

static esp_err_t light_sensor_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    i2c_master_bus_handle_t bus = NULL;

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus create failed");
    ESP_RETURN_ON_ERROR(bh1750_create(bus, BH1750_I2C_ADDRESS_DEFAULT, &s_bh1750), TAG,
                        "bh1750 not found");
    ESP_RETURN_ON_ERROR(bh1750_power_on(s_bh1750), TAG, "bh1750 power on failed");
    /* Continuous mode: the read callback never waits for a conversion. */
    return bh1750_set_measure_mode(s_bh1750, BH1750_CONTINUE_1LX_RES);
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "occ1", .model = "ex-occ"},
        .attribute_read = on_read,
    };

    ESP_ERROR_CHECK(light_sensor_init());

    ep = en2m_endpoint_create(ENDPOINT);
    if (ep == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_OCCUPANCY_SENSOR));
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_LIGHT_SENSOR));

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_ERROR_CHECK(pir_init());
    ESP_LOGI(TAG, "ready — motion is pushed, illuminance is pulled");
}
