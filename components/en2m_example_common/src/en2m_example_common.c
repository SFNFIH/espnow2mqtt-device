#include "en2m_example_common.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

static const char *TAG = "ex_common";

static void on_log(const char *msg, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "[mesh] %s", msg);
}

esp_err_t en2m_example_mesh_start(const en2m_example_cfg_t *cfg)
{
    if (!cfg || !cfg->name || !cfg->model) {
        return ESP_ERR_INVALID_ARG;
    }

    en2m_app_config_t mesh_cfg = {
        .role = cfg->role ? cfg->role : EN2M_ROLE_LEAF,
        .model = cfg->model,
        .name = cfg->name,
        .fw = cfg->fw ? cfg->fw : EN2M_FW_VERSION,
        .channel = cfg->channel ? cfg->channel : EN2M_WIFI_CHANNEL,
        .on_command = cfg->on_command,
        .on_log = on_log,
        .user = cfg->user,
    };
    return en2m_mesh_init(&mesh_cfg);
}

void en2m_example_mesh_tick(void)
{
    en2m_mesh_loop();
}

static esp_err_t send_json(uint8_t type, uint16_t cmd_id, const char *json_object)
{
    if (!json_object) {
        json_object = "{}";
    }
    return en2m_send_uplink(type, cmd_id, (const uint8_t *)json_object,
                            (uint8_t)strnlen(json_object, EN2M_DATA_MAX));
}

esp_err_t en2m_example_send_hello(const char *json_object)
{
    return send_json(EN2M_MSG_HELLO, 0, json_object);
}

esp_err_t en2m_example_send_state(const char *json_object)
{
    return send_json(EN2M_MSG_STATE, 0, json_object);
}

esp_err_t en2m_example_send_ack(uint16_t cmd_id, const char *json_object)
{
    return send_json(EN2M_MSG_ACK, cmd_id, json_object);
}

int en2m_example_build_base_json(char *buf, size_t buflen, const char *caps_csv,
                                 const char *extras)
{
    if (!buf || buflen < 16) {
        return -1;
    }

    char parent[18] = "none";
    int cost = -1;
    if (en2m_has_parent()) {
        uint8_t mac[6];
        en2m_parent_mac(mac);
        en2m_mac_to_str(mac, parent);
        cost = en2m_path_cost();
    }

    const char *role = "leaf";
    if (en2m_role() == EN2M_ROLE_ROUTER) {
        role = "router";
    } else if (en2m_role() == EN2M_ROLE_COORDINATOR) {
        role = "coordinator";
    }

    /* caps_csv "a,b" -> JSON array ["a","b"] */
    char caps_json[96] = "[]";
    if (caps_csv && caps_csv[0]) {
        char tmp[96];
        size_t o = 0;
        tmp[o++] = '[';
        const char *p = caps_csv;
        while (*p && o + 4 < sizeof(tmp)) {
            if (o > 1) {
                tmp[o++] = ',';
            }
            tmp[o++] = '"';
            while (*p && *p != ',' && o + 2 < sizeof(tmp)) {
                tmp[o++] = *p++;
            }
            tmp[o++] = '"';
            if (*p == ',') {
                p++;
            }
        }
        tmp[o++] = ']';
        tmp[o] = 0;
        strncpy(caps_json, tmp, sizeof(caps_json) - 1);
    }

    int n;
    if (cost >= 0) {
        n = snprintf(buf, buflen,
                     "{\"node_role\":\"%s\",\"path_cost\":%d,\"parent\":\"%s\",\"caps\":%s%s%s}",
                     role, cost, parent, caps_json,
                     extras && extras[0] ? "," : "", extras ? extras : "");
    } else {
        n = snprintf(buf, buflen,
                     "{\"node_role\":\"%s\",\"caps\":%s%s%s}",
                     role, caps_json,
                     extras && extras[0] ? "," : "", extras ? extras : "");
    }
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return n;
}

bool en2m_example_dht_read(int gpio, int dht_type, float *temp_c, float *hum)
{
    if (!temp_c || !hum) {
        return false;
    }

    gpio_set_direction(gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(gpio, 1);
    ets_delay_us(1000);
    gpio_set_level(gpio, 0);
    ets_delay_us(dht_type == 22 ? 1200 : 20000);
    gpio_set_level(gpio, 1);
    ets_delay_us(40);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);

    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(gpio) == 0) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(gpio) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return false;
        }
    }

    uint8_t data[5] = {0};
    for (int i = 0; i < 40; i++) {
        while (gpio_get_level(gpio) == 0) {
        }
        int64_t t0 = esp_timer_get_time();
        while (gpio_get_level(gpio) == 1) {
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

    if (dht_type == 22) {
        *hum = ((data[0] << 8) | data[1]) * 0.1f;
        int16_t t = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
        *temp_c = t * 0.1f;
        if (data[2] & 0x80) {
            *temp_c = -*temp_c;
        }
    } else {
        *hum = data[0];
        *temp_c = data[2];
    }
    return true;
}
