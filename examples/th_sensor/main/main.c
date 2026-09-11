/**
 * Temperature / humidity sensor — pull-style sensor, zero application code
 * outside the read callback.
 *
 * The component calls on_read() once per attribute right before it builds a
 * report, so the sensor is sampled exactly as often as the mesh needs it.
 * Temperature and humidity arrive in the same AHT20 measurement, so the result
 * is cached for a moment and the second callback is served from the cache
 * instead of paying for a second conversion.
 *
 * The sensor is `espressif/aht20`, which pulls in `espressif/i2c_bus`. The
 * AHT20 is the modern I2C replacement for the one-wire DHT22: same job, no
 * bit-banged timing, and a driver that is maintained for us.
 */
#include "aht20.h"
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#define PIN_SDA 4
#define PIN_SCL 5
#define I2C_PORT I2C_NUM_0
#define I2C_HZ 100000
/** One measurement serves both attributes of a report. */
#define SAMPLE_CACHE_MS 2000
#define ENDPOINT 1

static const char *TAG = "ex_th";

static aht20_dev_handle_t s_aht20;

static struct {
    int64_t taken_us;
    float celsius;
    float humidity;
} s_sample;

/** Measures unless the cached reading is still fresh enough to reuse. */
static esp_err_t sample(void)
{
    int64_t now = esp_timer_get_time();
    uint32_t t_raw, h_raw;
    float celsius, humidity;

    if (s_sample.taken_us != 0 && now - s_sample.taken_us < SAMPLE_CACHE_MS * 1000LL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(aht20_read_temperature_humidity(s_aht20, &t_raw, &celsius, &h_raw, &humidity),
                        TAG, "aht20 read failed");
    s_sample.celsius = celsius;
    s_sample.humidity = humidity;
    s_sample.taken_us = now;
    return ESP_OK;
}

static esp_err_t on_read(const en2m_attr_path_t *path, en2m_value_t *out_value, void *ctx)
{
    (void)ctx;

    switch (path->cluster_id) {
    case EN2M_CLUSTER_TEMPERATURE_MEASUREMENT:
        ESP_RETURN_ON_ERROR(sample(), TAG, "temperature read failed");
        *out_value = en2m_i16((int16_t)(s_sample.celsius * 100.0f));
        return ESP_OK;
    case EN2M_CLUSTER_RELATIVE_HUMIDITY:
        ESP_RETURN_ON_ERROR(sample(), TAG, "humidity read failed");
        *out_value = en2m_u16((uint16_t)(s_sample.humidity * 100.0f));
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t sensor_init(void)
{
    const i2c_config_t bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master = {.clk_speed = I2C_HZ},
    };
    i2c_bus_handle_t bus = i2c_bus_create(I2C_PORT, &bus_cfg);
    aht20_i2c_config_t dev_cfg = {
        .bus_inst = bus,
        .i2c_addr = AHT20_ADDRRES_0,
    };

    ESP_RETURN_ON_FALSE(bus != NULL, ESP_FAIL, TAG, "i2c bus create failed");
    return aht20_new_sensor(&dev_cfg, &s_aht20);
}

void app_main(void)
{
    en2m_endpoint_t *ep;
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "th_sensor1", .model = "ex-th"},
        .attribute_read = on_read,
        .report_interval_ms = 60000,
        .min_report_interval_ms = 5000,
    };

    ESP_ERROR_CHECK(sensor_init());

    ep = en2m_endpoint_create(ENDPOINT);
    if (ep == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_TEMPERATURE_SENSOR));
    ESP_ERROR_CHECK(en2m_endpoint_add_device_type(ep, EN2M_DEVICE_TYPE_HUMIDITY_SENSOR));

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — sampling happens on demand, not in a loop");
}
