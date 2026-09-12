/**
 * Scene switch / wireless button — Switch cluster.
 *
 * A button has no state to hold: by the time the report lands, the press is
 * over. So this node reports a press *counter* plus the kind of the last press,
 * and Home Assistant turns a change of the counter into an event. The counter
 * is not optional — state reports are full snapshots on a retained topic, so
 * two presses that looked identical would be indistinguishable from one report
 * arriving twice. ::en2m_report_button keeps the counter for us.
 *
 * The four gestures come straight from `espressif/button`, which already
 * debounces and classifies single click, double click, hold and release. The
 * mapping is carried in usr_data, so one callback serves all four.
 *
 * Nothing here is periodic. `EN2M_REPORT_ON_CHANGE_ONLY` means the radio only
 * wakes for an actual press, which matters for a battery-powered switch.
 */
#include "button_gpio.h"
#include "driver/gpio.h"
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "iot_button.h"

#define PIN_BUTTON GPIO_NUM_9
#define BUTTON_ACTIVE_LEVEL 0
/* How long the component waits after a release for another press before it
 * declares a single click. Also the maximum gap inside a double click. */
#define CLICK_GAP_MS 300
#define LONG_PRESS_MS 800
#define ENDPOINT 1

static const char *TAG = "ex_scene";

/** Runs on the en2m task, which is the only place the press counter advances. */
static void report_press(void *arg)
{
    en2m_press_action_t action = (en2m_press_action_t)(uintptr_t)arg;

    ESP_LOGI(TAG, "press action %d", (int)action);
    en2m_report_button(ENDPOINT, action);
}

static void on_press(void *button_handle, void *usr_data)
{
    (void)button_handle;
    en2m_schedule(report_press, usr_data);
}

static esp_err_t button_init(void)
{
    static const struct {
        button_event_t event;
        en2m_press_action_t action;
    } map[] = {
        {BUTTON_SINGLE_CLICK, EN2M_PRESS_SHORT},
        {BUTTON_DOUBLE_CLICK, EN2M_PRESS_DOUBLE},
        {BUTTON_LONG_PRESS_START, EN2M_PRESS_LONG},
        {BUTTON_LONG_PRESS_UP, EN2M_PRESS_RELEASE},
    };
    const button_config_t btn_cfg = {
        .long_press_time = LONG_PRESS_MS,
        .short_press_time = CLICK_GAP_MS,
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = PIN_BUTTON,
        .active_level = BUTTON_ACTIVE_LEVEL,
    };
    button_handle_t btn = NULL;

    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn), TAG,
                        "button init failed");
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, map[i].event, NULL, on_press,
                                                   (void *)(uintptr_t)map[i].action),
                            TAG, "cannot register event %d", (int)map[i].event);
    }
    return ESP_OK;
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "switch1", .model = "ex-scene"},
        .report_mode = EN2M_REPORT_ON_CHANGE_ONLY,
    };

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_GENERIC_SWITCH) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_ERROR_CHECK(button_init());
    ESP_LOGI(TAG, "ready — press, double press, or hold GPIO%d", PIN_BUTTON);
}
