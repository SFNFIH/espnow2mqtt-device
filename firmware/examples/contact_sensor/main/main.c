/**
 * Example: Contact / door window sensor (report-only).
 *
 * - Role: LEAF
 * - Caps: contact
 * - GPIO pulled-up; closed to GND => OFF (closed), open => ON (open)
 *   (Home Assistant binary_sensor door: ON=open)
 *
 * Wiring:
 *   Reed switch / button between GPIO9 and GND (uses internal pull-up)
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "en2m_example_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef PIN_CONTACT
#define PIN_CONTACT 9
#endif

static const char *TAG = "contact";
static const char *CAPS = "contact";

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static bool contact_open(void)
{
    /* pull-up: 1 = open circuit = door open */
    return gpio_get_level(PIN_CONTACT) == 1;
}

static void publish(bool hello, const char *event)
{
    char extras[96];
    if (event && event[0]) {
        snprintf(extras, sizeof(extras),
                 "\"contact\":\"%s\",\"event\":\"%s\"",
                 contact_open() ? "ON" : "OFF", event);
    } else {
        snprintf(extras, sizeof(extras), "\"contact\":\"%s\"",
                 contact_open() ? "ON" : "OFF");
    }
    char json[EN2M_DATA_MAX];
    if (en2m_example_build_base_json(json, sizeof(json), CAPS, extras) < 0) {
        return;
    }
    if (hello) {
        en2m_example_send_hello(json);
    } else {
        en2m_example_send_state(json);
    }
}

static void app_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_CONTACT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    gpio_config(&io);

    bool last = contact_open();
    int64_t last_hb = 0;
    publish(true, NULL);
    publish(false, NULL);

    while (1) {
        en2m_example_mesh_tick();
        bool now = contact_open();
        if (now != last) {
            last = now;
            publish(false, now ? "opened" : "closed");
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (now_ms() - last_hb > 60000) {
            last_hb = now_ms();
            publish(false, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "example: contact sensor (report-only)");
    en2m_example_cfg_t cfg = {
        .role = EN2M_ROLE_LEAF,
        .name = "door1",
        .model = "ex-contact",
        .fw = "0.3.0-idf",
        .on_command = NULL,
    };
    ESP_ERROR_CHECK(en2m_example_mesh_start(&cfg));
    xTaskCreate(app_task, "contact", 6144, NULL, 4, NULL);
}
