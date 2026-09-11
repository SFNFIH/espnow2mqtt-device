#include "en2m_device_app.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "en2m_mesh.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rom/ets_sys.h"

#ifndef PIN_DHT
#define PIN_DHT 4
#endif
#ifndef PIN_RELAY
#define PIN_RELAY 5
#endif
#ifndef PIN_BUTTON
#define PIN_BUTTON 9
#endif
#ifndef DHT_TYPE
#define DHT_TYPE 22
#endif

static const char *TAG = "device_app";
static bool s_relay_on;
static char s_name[16];
static char s_model[12];
static uint8_t s_role;

static int64_t millis(void) { return esp_timer_get_time() / 1000; }

#if DHT_TYPE >= 0
static bool dht_read(float *temp_c, float *hum)
{
    gpio_set_direction(PIN_DHT, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(PIN_DHT, 1);
    ets_delay_us(1000);
    gpio_set_level(PIN_DHT, 0);
    ets_delay_us(DHT_TYPE == 22 ? 1200 : 20000);
    gpio_set_level(PIN_DHT, 1);
    ets_delay_us(40);
    gpio_set_direction(PIN_DHT, GPIO_MODE_INPUT);

    int64_t start = esp_timer_get_time();
    while (gpio_get_level(PIN_DHT) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(PIN_DHT) == 0) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(PIN_DHT) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }

    uint8_t data[5] = {0};
    for (int i = 0; i < 40; i++) {
        while (gpio_get_level(PIN_DHT) == 0) {
        }
        int64_t t0 = esp_timer_get_time();
        while (gpio_get_level(PIN_DHT) == 1) {
        }
        int64_t high = esp_timer_get_time() - t0;
        data[i / 8] <<= 1;
        if (high > 50) {
            data[i / 8] |= 1;
        }
    }
    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4]) {
        return false;
    }
#if DHT_TYPE == 22
    *hum = ((data[0] << 8) | data[1]) * 0.1f;
    int16_t t = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
    *temp_c = t * 0.1f;
    if (data[2] & 0x80) {
        *temp_c = -*temp_c;
    }
#else
    *hum = data[0];
    *temp_c = data[2];
#endif
    return true;
}
#endif

static void apply_relay(bool on)
{
    s_relay_on = on;
    gpio_set_level(PIN_RELAY, s_relay_on ? 1 : 0);
    nvs_handle_t h;
    if (nvs_open("en2m", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "relay", s_relay_on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static char *build_state_json(bool with_button, const char *btn)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "switch", s_relay_on ? "ON" : "OFF");
    cJSON_AddStringToObject(doc, "node_role", s_role == EN2M_ROLE_ROUTER ? "router" : "leaf");
    if (en2m_has_parent()) {
        cJSON_AddNumberToObject(doc, "path_cost", en2m_path_cost());
        uint8_t p[6];
        char ps[18];
        en2m_parent_mac(p);
        en2m_mac_to_str(p, ps);
        cJSON_AddStringToObject(doc, "parent", ps);
    }
#if DHT_TYPE >= 0
    float t = 0, h = 0;
    if (dht_read(&t, &h)) {
        cJSON_AddNumberToObject(doc, "temperature", ((int)(t * 10)) / 10.0);
        cJSON_AddNumberToObject(doc, "humidity", ((int)(h * 10)) / 10.0);
    }
#endif
    if (with_button && btn) {
        cJSON_AddStringToObject(doc, "button", btn);
    }
    char *s = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    return s;
}

static void send_state(bool with_button, const char *btn)
{
    char *s = build_state_json(with_button, btn);
    if (!s) {
        return;
    }
    en2m_send_uplink(EN2M_MSG_STATE, 0, (const uint8_t *)s,
                     (uint8_t)strnlen(s, EN2M_DATA_MAX));
    cJSON_free(s);
}

static void send_hello(void)
{
    char *s = build_state_json(false, NULL);
    if (!s) {
        return;
    }
    en2m_send_uplink(EN2M_MSG_HELLO, 0, (const uint8_t *)s,
                     (uint8_t)strnlen(s, EN2M_DATA_MAX));
    cJSON_free(s);
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
            apply_relay(true);
        } else if (strcmp(sw->valuestring, "OFF") == 0) {
            apply_relay(false);
        } else if (strcmp(sw->valuestring, "TOGGLE") == 0) {
            apply_relay(!s_relay_on);
        }
    }
    if (doc) {
        cJSON_Delete(doc);
    }
    char *ack = build_state_json(false, NULL);
    if (ack) {
        en2m_send_uplink(EN2M_MSG_ACK, pkt->cmd_id, (const uint8_t *)ack,
                         (uint8_t)strnlen(ack, EN2M_DATA_MAX));
        cJSON_free(ack);
    }
    send_state(false, NULL);
}

static void on_log(const char *msg, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "[mesh] %s", msg);
}

static void app_task(void *arg)
{
    (void)arg;
    int64_t last_tel = 0;
    int64_t last_hello = 0;
    int last_btn = 1;
    const int64_t tel_ms = (s_role == EN2M_ROLE_LEAF) ? 30000 : 15000;

    send_hello();
    send_state(false, NULL);

    while (1) {
        en2m_mesh_loop();
        int btn = gpio_get_level(PIN_BUTTON);
        if (last_btn == 1 && btn == 0) {
            apply_relay(!s_relay_on);
            send_state(true, "single");
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        last_btn = btn;

        int64_t now = millis();
        if (now - last_tel > tel_ms) {
            last_tel = now;
            send_state(false, NULL);
        }
        if (now - last_hello > 45000) {
            last_hello = now;
            send_hello();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t en2m_device_app_start(uint8_t role, const char *name, const char *model)
{
    if (role != EN2M_ROLE_LEAF && role != EN2M_ROLE_ROUTER) {
        return ESP_ERR_INVALID_ARG;
    }
    s_role = role;
    strncpy(s_name, name && name[0] ? name : EN2M_DEVICE_NAME, sizeof(s_name) - 1);
    strncpy(s_model, model && model[0] ? model : EN2M_DEVICE_MODEL, sizeof(s_model) - 1);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_config_t out = {
        .pin_bit_mask = 1ULL << PIN_RELAY,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    gpio_config(&in);

    nvs_handle_t h;
    if (nvs_open("en2m", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "relay", &v) == ESP_OK) {
            s_relay_on = v != 0;
        }
        size_t len = sizeof(s_name);
        nvs_get_str(h, "name", s_name, &len);
        nvs_close(h);
    }
    gpio_set_level(PIN_RELAY, s_relay_on ? 1 : 0);

    en2m_app_config_t cfg = {
        .role = s_role,
        .model = s_model,
        .name = s_name,
        .fw = EN2M_FW_VERSION,
        .channel = EN2M_WIFI_CHANNEL,
        .on_command = on_command,
        .on_log = on_log,
        .user_ctx = NULL,
    };
    err = en2m_mesh_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    en2m_set_name(s_name);

    ESP_LOGI(TAG, "role=%s name=%s model=%s",
             s_role == EN2M_ROLE_ROUTER ? "router" : "leaf", s_name, s_model);
    xTaskCreate(app_task, "device", 8192, NULL, 4, NULL);
    return ESP_OK;
}
