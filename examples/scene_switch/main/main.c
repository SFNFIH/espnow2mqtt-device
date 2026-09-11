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
 * Nothing here is periodic. `EN2M_REPORT_ON_CHANGE_ONLY` means the radio only
 * wakes for an actual press, which matters for a battery-powered switch.
 */
#include "en2m.h"
#include "esp_log.h"

#include "../../../drivers/drv_gpio_button_gesture.h"

#define PIN_BUTTON GPIO_NUM_9
#define BUTTON_ACTIVE_LOW true
#define DEBOUNCE_MS 30
#define LONG_PRESS_MS 800
#define DOUBLE_GAP_MS 300
#define ENDPOINT 1

static const char *TAG = "ex_scene";

static void on_gesture(drv_gpio_button_gesture_t gesture, void *ctx)
{
    en2m_press_action_t action;

    (void)ctx;
    switch (gesture) {
    case DRV_BUTTON_DOUBLE_PRESS:
        action = EN2M_PRESS_DOUBLE;
        break;
    case DRV_BUTTON_LONG_PRESS:
        action = EN2M_PRESS_LONG;
        break;
    case DRV_BUTTON_RELEASE:
        action = EN2M_PRESS_RELEASE;
        break;
    case DRV_BUTTON_SHORT_PRESS:
    default:
        action = EN2M_PRESS_SHORT;
        break;
    }
    ESP_LOGI(TAG, "gesture %d", (int)gesture);
    en2m_report_button(ENDPOINT, action);
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

    ESP_ERROR_CHECK(drv_gpio_button_gesture_init(PIN_BUTTON, BUTTON_ACTIVE_LOW, DEBOUNCE_MS,
                                                 LONG_PRESS_MS, DOUBLE_GAP_MS, on_gesture, NULL));
    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — press, double press, or hold GPIO%d", PIN_BUTTON);
}
