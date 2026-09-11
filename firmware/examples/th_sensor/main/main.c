/**
 * Example: Temperature & Humidity sensor (report-only).
 *
 * - Role: LEAF (no forwarding)
 * - Caps: temperature,humidity
 * - No command handler — pure uplink telemetry
 *
 * Wiring (default):
 *   DHT22 DATA -> GPIO4, VCC 3V3, GND, 4.7k~10k pull-up on DATA
 */
#include <stdio.h>
#include <string.h>

#include "en2m_example_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef PIN_DHT
#define PIN_DHT 4
#endif
#ifndef DHT_TYPE
#define DHT_TYPE 22
#endif

static const char *TAG = "th_sensor";
static const char *CAPS = "temperature,humidity";

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void publish(bool hello)
{
    float t = 0, h = 0;
    char extras[80] = {0};
    if (en2m_example_dht_read(PIN_DHT, DHT_TYPE, &t, &h)) {
        snprintf(extras, sizeof(extras),
                 "\"temperature\":%.1f,\"humidity\":%.1f", t, h);
    } else {
        snprintf(extras, sizeof(extras), "\"temperature\":null,\"humidity\":null");
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
    int64_t last = 0;
    publish(true);
    publish(false);
    while (1) {
        en2m_example_mesh_tick();
        if (now_ms() - last > 30000) {
            last = now_ms();
            publish(false);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "example: temperature/humidity (report-only)");
    en2m_example_cfg_t cfg = {
        .role = EN2M_ROLE_LEAF,
        .name = "th_sensor1",
        .model = "ex-th",
        .fw = "0.3.0-idf",
        .on_command = NULL, /* report-only */
    };
    ESP_ERROR_CHECK(en2m_example_mesh_start(&cfg));
    xTaskCreate(app_task, "th", 6144, NULL, 4, NULL);
}
