/**
 * Example: Smart plug style device (report + execute).
 *
 * - Caps: switch,power,energy  (power/energy are mocked counters for demo)
 * - Commands: {"switch":"ON"|"OFF"|"TOGGLE"}
 * - Reports instantaneous power_w and cumulative energy_wh while ON
 *
 * Wiring: same as relay_switch (GPIO5 relay, GPIO9 button)
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "en2m_example_common.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifndef PIN_RELAY
#define PIN_RELAY 5
#endif
#ifndef PIN_BUTTON
#define PIN_BUTTON 9
#endif

static const char *TAG = "smart_plug";
static const char *CAPS = "switch,power,energy";
static bool s_on;
static float s_energy_wh;
static float s_power_w;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void apply(bool on)
{
    s_on = on;
    gpio_set_level(PIN_RELAY, s_on ? 1 : 0);
    if (!s_on) {
        s_power_w = 0;
    }
    nvs_handle_t h;
    if (nvs_open("ex_plug", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "on", s_on ? 1 : 0);
        nvs_set_blob(h, "wh", &s_energy_wh, sizeof(s_energy_wh));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void publish(bool hello)
{
    char extras[120];
    snprintf(extras, sizeof(extras),
             "\"switch\":\"%s\",\"power\":%.1f,\"energy\":%.2f",
             s_on ? "ON" : "OFF", s_power_w, s_energy_wh);
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

static void on_command(const en2m_pkt_t *pkt, void *user)
{
    (void)user;
    char tmp[EN2M_DATA_MAX + 1] = {0};
    if (pkt->data_len) {
        memcpy(tmp, pkt->data, pkt->data_len);
    }
    cJSON *doc = cJSON_Parse(tmp);
    const cJSON *sw = doc ? cJSON_GetObjectItem(doc, "switch") : NULL;
    if (cJSON_IsString(sw)) {
        if (strcmp(sw->valuestring, "ON") == 0) {
            apply(true);
        } else if (strcmp(sw->valuestring, "OFF") == 0) {
            apply(false);
        } else if (strcmp(sw->valuestring, "TOGGLE") == 0) {
            apply(!s_on);
        }
    }
    if (doc) {
        cJSON_Delete(doc);
    }
    char extras[120];
    snprintf(extras, sizeof(extras),
             "\"switch\":\"%s\",\"power\":%.1f,\"energy\":%.2f",
             s_on ? "ON" : "OFF", s_power_w, s_energy_wh);
    char json[EN2M_DATA_MAX];
    if (en2m_example_build_base_json(json, sizeof(json), CAPS, extras) >= 0) {
        en2m_example_send_ack(pkt->cmd_id, json);
        en2m_example_send_state(json);
    }
}

static void app_task(void *arg)
{
    (void)arg;
    int last_btn = 1;
    int64_t last_tel = 0;
    int64_t last_energy = now_ms();
    publish(true);
    publish(false);

    while (1) {
        en2m_example_mesh_tick();

        int btn = gpio_get_level(PIN_BUTTON);
        if (last_btn == 1 && btn == 0) {
            apply(!s_on);
            publish(false);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        last_btn = btn;

        int64_t now = now_ms();
        if (s_on) {
            /* Mock load ~20–60 W */
            s_power_w = 20.0f + (esp_random() % 400) / 10.0f;
            float dt_h = (now - last_energy) / 3600000.0f;
            s_energy_wh += s_power_w * dt_h;
        }
        last_energy = now;

        if (now - last_tel > 15000) {
            last_tel = now;
            publish(false);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "example: smart plug (switch + power report)");
    ESP_ERROR_CHECK(nvs_flash_init());

    gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_RELAY, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    gpio_config(&in);

    nvs_handle_t h;
    if (nvs_open("ex_plug", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "on", &v) == ESP_OK) {
            s_on = v != 0;
        }
        size_t len = sizeof(s_energy_wh);
        nvs_get_blob(h, "wh", &s_energy_wh, &len);
        nvs_close(h);
    }
    gpio_set_level(PIN_RELAY, s_on ? 1 : 0);

    en2m_example_cfg_t cfg = {
        .role = EN2M_ROLE_LEAF,
        .name = "plug1",
        .model = "ex-plug",
        .fw = "0.3.0-idf",
        .on_command = on_command,
    };
    ESP_ERROR_CHECK(en2m_example_mesh_start(&cfg));
    xTaskCreate(app_task, "plug", 6144, NULL, 4, NULL);
}
