/**
 * @file en2m_example_common.c
 * @brief Shared helpers for device examples.
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "en2m_example_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

static const char *TAG = "en2m_ex";

static void en2m_example_log(const char *msg, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "[mesh] %s", msg);
}

esp_err_t en2m_example_mesh_start(const en2m_example_config_t *config)
{
    en2m_config_t mesh_config = {0};

    if (config == NULL || config->name == NULL || config->model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mesh_config.role = config->role ? config->role : EN2M_ROLE_LEAF;
    mesh_config.model = config->model;
    mesh_config.name = config->name;
    mesh_config.fw = (config->fw != NULL) ? config->fw : EN2M_FW_VERSION;
    mesh_config.channel = config->channel ? config->channel : EN2M_WIFI_CHANNEL;
    mesh_config.on_command = config->on_command;
    mesh_config.on_log = en2m_example_log;
    mesh_config.user_ctx = config->user_ctx;

    return en2m_mesh_init(&mesh_config);
}

void en2m_example_mesh_tick(void)
{
    en2m_mesh_loop();
}

static esp_err_t en2m_example_send_json(uint8_t type, uint16_t cmd_id, const char *json_object)
{
    if (json_object == NULL) {
        json_object = "{}";
    }
    return en2m_send_uplink(type, cmd_id, (const uint8_t *)json_object,
                            (uint8_t)strnlen(json_object, EN2M_DATA_MAX));
}

esp_err_t en2m_example_send_hello(const char *json_object)
{
    return en2m_example_send_json(EN2M_MSG_HELLO, 0, json_object);
}

esp_err_t en2m_example_send_state(const char *json_object)
{
    return en2m_example_send_json(EN2M_MSG_STATE, 0, json_object);
}

esp_err_t en2m_example_send_ack(uint16_t cmd_id, const char *json_object)
{
    return en2m_example_send_json(EN2M_MSG_ACK, cmd_id, json_object);
}

int en2m_example_build_base_json(char *buf, size_t buflen, const char *caps_csv, const char *extras)
{
    char parent[18] = "none";
    char caps_json[96] = "[]";
    int cost = -1;
    const char *role = "leaf";
    int written;

    if (buf == NULL || buflen < 16) {
        return -1;
    }

    if (en2m_has_parent()) {
        uint8_t mac[6];
        en2m_get_parent_mac(mac);
        en2m_mac_to_str(mac, parent);
        cost = en2m_get_path_cost();
    }

    if (en2m_get_role() == EN2M_ROLE_ROUTER) {
        role = "router";
    } else if (en2m_get_role() == EN2M_ROLE_COORDINATOR) {
        role = "coordinator";
    }

    if (caps_csv != NULL && caps_csv[0] != '\0') {
        char tmp[96];
        size_t out = 0;
        const char *p = caps_csv;

        tmp[out++] = '[';
        while (*p != '\0' && out + 4 < sizeof(tmp)) {
            if (out > 1) {
                tmp[out++] = ',';
            }
            tmp[out++] = '"';
            while (*p != '\0' && *p != ',' && out + 2 < sizeof(tmp)) {
                tmp[out++] = *p++;
            }
            tmp[out++] = '"';
            if (*p == ',') {
                p++;
            }
        }
        tmp[out++] = ']';
        tmp[out] = '\0';
        strncpy(caps_json, tmp, sizeof(caps_json) - 1);
    }

    if (cost >= 0) {
        written = snprintf(buf, buflen,
                           "{\"node_role\":\"%s\",\"path_cost\":%d,\"parent\":\"%s\",\"caps\":%s%s%s}",
                           role, cost, parent, caps_json,
                           (extras != NULL && extras[0] != '\0') ? "," : "",
                           (extras != NULL) ? extras : "");
    } else {
        written = snprintf(buf, buflen,
                           "{\"node_role\":\"%s\",\"caps\":%s%s%s}",
                           role, caps_json,
                           (extras != NULL && extras[0] != '\0') ? "," : "",
                           (extras != NULL) ? extras : "");
    }

    if (written < 0 || (size_t)written >= buflen) {
        return -1;
    }
    return written;
}

bool en2m_example_dht_read(int gpio_num, int dht_type, float *temp_c, float *humidity)
{
    uint8_t data[5] = {0};
    int64_t start;

    if (temp_c == NULL || humidity == NULL) {
        return false;
    }

    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(gpio_num, 1);
    ets_delay_us(1000);
    gpio_set_level(gpio_num, 0);
    ets_delay_us(dht_type == 22 ? 1200 : 20000);
    gpio_set_level(gpio_num, 1);
    ets_delay_us(40);
    gpio_set_direction(gpio_num, GPIO_MODE_INPUT);

    start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 0) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }

    for (int i = 0; i < 40; i++) {
        int64_t t0;
        int64_t high_us;

        while (gpio_get_level(gpio_num) == 0) {
        }
        t0 = esp_timer_get_time();
        while (gpio_get_level(gpio_num) == 1) {
        }
        high_us = esp_timer_get_time() - t0;
        data[i / 8] <<= 1;
        if (high_us > 50) {
            data[i / 8] |= 1;
        }
    }

    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4]) {
        return false;
    }

    if (dht_type == 22) {
        int16_t raw_t = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
        *humidity = ((data[0] << 8) | data[1]) * 0.1f;
        *temp_c = raw_t * 0.1f;
        if (data[2] & 0x80) {
            *temp_c = -(*temp_c);
        }
    } else {
        *humidity = data[0];
        *temp_c = data[2];
    }
    return true;
}
