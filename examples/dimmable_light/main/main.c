/**
 * Tunable white light on a WS2812 strip — OnOff + Level + ColorControl.
 *
 * One write callback covers all three clusters, because all three end up in the
 * same place: one RGB value pushed out to the strip.
 *
 * The strip is driven by `espressif/led_strip` over RMT. WS2812 pixels are RGB,
 * while the Matter data model speaks mireds, so ::mireds_to_rgb walks a small
 * blackbody table to turn a colour temperature into something the strip can
 * actually show.
 */
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

/* GPIO8 is the addressable LED already fitted to the ESP32-C3-DevKitM-1 and
 * DevKitC-02, so the default build lights up with nothing wired at all. */
#define PIN_STRIP GPIO_NUM_8
#define STRIP_LEDS 1
#define RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define ENDPOINT 1

static const char *TAG = "ex_light";

static led_strip_handle_t s_strip;

static struct {
    bool on;
    uint8_t level;
    uint16_t mireds;
} s_light = {.on = false, .level = 254, .mireds = 300};

/**
 * Blackbody curve, coarse but enough for a light: 2000 K is 500 mireds and
 * 6500 K is 154, which is exactly the span Home Assistant offers. Red is
 * saturated across the whole range, so only green and blue actually move.
 */
static const struct {
    uint16_t kelvin;
    uint8_t r, g, b;
} s_blackbody[] = {
    {2000, 255, 141, 11},  {2500, 255, 165, 71},  {3000, 255, 180, 107},
    {3500, 255, 196, 137}, {4000, 255, 209, 163}, {4500, 255, 219, 186},
    {5000, 255, 228, 206}, {5500, 255, 236, 224}, {6000, 255, 243, 239},
    {6500, 255, 249, 253},
};

#define BLACKBODY_COUNT (sizeof(s_blackbody) / sizeof(s_blackbody[0]))

static uint8_t lerp_u8(uint8_t from, uint8_t to, uint32_t pos, uint32_t span)
{
    int32_t delta = (int32_t)to - (int32_t)from;

    if (span == 0) {
        return to;
    }
    return (uint8_t)((int32_t)from + delta * (int32_t)pos / (int32_t)span);
}

static void mireds_to_rgb(uint16_t mireds, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t kelvin = (mireds > 0) ? (1000000u / mireds) : s_blackbody[BLACKBODY_COUNT - 1].kelvin;
    size_t hi;

    for (hi = 1; hi < BLACKBODY_COUNT; hi++) {
        if (kelvin <= s_blackbody[hi].kelvin) {
            break;
        }
    }
    if (hi >= BLACKBODY_COUNT) {
        hi = BLACKBODY_COUNT - 1;
    }

    const uint32_t lo_k = s_blackbody[hi - 1].kelvin;
    const uint32_t span = s_blackbody[hi].kelvin - lo_k;
    const uint32_t pos = (kelvin > lo_k) ? (kelvin - lo_k) : 0;

    *r = lerp_u8(s_blackbody[hi - 1].r, s_blackbody[hi].r, pos, span);
    *g = lerp_u8(s_blackbody[hi - 1].g, s_blackbody[hi].g, pos, span);
    *b = lerp_u8(s_blackbody[hi - 1].b, s_blackbody[hi].b, pos, span);
}

static void light_apply(void)
{
    uint8_t r, g, b;
    uint32_t gamma_level;

    if (!s_light.on || s_light.level == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_clear(s_strip));
        ESP_LOGI(TAG, "output: off");
        return;
    }

    mireds_to_rgb(s_light.mireds, &r, &g, &b);

    /* A WS2812's duty cycle is linear and the eye is not, so scaling the
     * channels straight by `level` makes the bottom of the slider look dead and
     * the top look flat. Squaring the brightness first (gamma ~2.0) spreads the
     * perceived steps out. It is applied to the brightness rather than to each
     * channel so that the colour ratio — the colour temperature — survives. */
    gamma_level = (uint32_t)s_light.level * s_light.level / 254u;
    r = (uint8_t)((uint32_t)r * gamma_level / 254u);
    g = (uint8_t)((uint32_t)g * gamma_level / 254u);
    b = (uint8_t)((uint32_t)b * gamma_level / 254u);

    for (uint32_t i = 0; i < STRIP_LEDS; i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_set_pixel(s_strip, i, r, g, b));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_refresh(s_strip));
    ESP_LOGI(TAG, "output: on level=%u mireds=%u rgb=%u,%u,%u", s_light.level, s_light.mireds, r, g,
             b);
}

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)ctx;

    switch (path->cluster_id) {
    case EN2M_CLUSTER_ON_OFF:
        s_light.on = value->v.b;
        break;
    case EN2M_CLUSTER_LEVEL_CONTROL:
        s_light.level = value->v.u8;
        break;
    case EN2M_CLUSTER_COLOR_CONTROL:
        s_light.mireds = value->v.u16;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    light_apply();
    return ESP_OK;
}

/** Identify makes one node stand out in a room full of identical ones. */
static void on_identify(uint8_t endpoint_id, uint16_t seconds, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "identify endpoint %u, %u s left", endpoint_id, seconds);
}

static esp_err_t strip_init(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_STRIP,
        .max_leds = STRIP_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG,
                        "led_strip init failed");
    return led_strip_clear(s_strip);
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "light1", .model = "ex-light"},
        .attribute_write = on_write,
        .identify = on_identify,
    };
    en2m_endpoint_t *ep;

    /* Before en2m_start, which replays the persisted brightness and colour
     * temperature through on_write() and therefore needs a working strip. */
    ESP_ERROR_CHECK(strip_init());

    ep = en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_COLOR_TEMPERATURE_LIGHT);
    if (ep == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }
    en2m_cluster_create(ep, EN2M_CLUSTER_IDENTIFY);

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — brightness and colour temperature arrive as writes");
}
