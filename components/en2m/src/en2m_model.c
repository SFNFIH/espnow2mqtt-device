/**
 * @file en2m_model.c
 * @brief Endpoint / cluster interaction layer (no hardware drivers).
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "en2m_model.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "en2m_model";

typedef struct {
    bool present;
    en2m_on_off_driver_t on_off;
    en2m_level_driver_t level;
    en2m_boolean_state_driver_t boolean_state;
    en2m_temperature_driver_t temperature;
    en2m_humidity_driver_t humidity;
    en2m_electrical_power_driver_t electrical;
} en2m_endpoint_clusters_t;

struct en2m_endpoint {
    bool used;
    uint8_t id;
    en2m_endpoint_clusters_t clusters;
};

static struct {
    bool started;
    en2m_endpoint_t endpoints[EN2M_MAX_ENDPOINTS];
    int64_t last_report_us;
    uint32_t report_interval_ms;
} s_model;

static int64_t en2m_model_now_us(void)
{
    return esp_timer_get_time();
}

static en2m_endpoint_t *en2m_endpoint_find(uint8_t endpoint_id)
{
    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        if (s_model.endpoints[i].used && s_model.endpoints[i].id == endpoint_id) {
            return &s_model.endpoints[i];
        }
    }
    return NULL;
}

en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id)
{
    if (endpoint_id == 0 || endpoint_id == 255) {
        ESP_LOGE(TAG, "invalid endpoint id %u", endpoint_id);
        return NULL;
    }
    if (en2m_endpoint_find(endpoint_id) != NULL) {
        ESP_LOGE(TAG, "endpoint %u already exists", endpoint_id);
        return NULL;
    }

    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        if (!s_model.endpoints[i].used) {
            memset(&s_model.endpoints[i], 0, sizeof(s_model.endpoints[i]));
            s_model.endpoints[i].used = true;
            s_model.endpoints[i].id = endpoint_id;
            return &s_model.endpoints[i];
        }
    }

    ESP_LOGE(TAG, "no free endpoint slot");
    return NULL;
}

esp_err_t en2m_endpoint_add_on_off(en2m_endpoint_t *ep, const en2m_on_off_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL && driver->get != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ep->clusters.present = true;
    ep->clusters.on_off = *driver;
    return ESP_OK;
}

esp_err_t en2m_endpoint_add_level_control(en2m_endpoint_t *ep, const en2m_level_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL && driver->get_level != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ep->clusters.present = true;
    ep->clusters.level = *driver;
    return ESP_OK;
}

esp_err_t en2m_endpoint_add_boolean_state(en2m_endpoint_t *ep, const en2m_boolean_state_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL && driver->get != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ep->clusters.present = true;
    ep->clusters.boolean_state = *driver;
    return ESP_OK;
}

esp_err_t en2m_endpoint_add_temperature(en2m_endpoint_t *ep, const en2m_temperature_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL && driver->get_measured_value != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bad args");
    ep->clusters.present = true;
    ep->clusters.temperature = *driver;
    return ESP_OK;
}

esp_err_t en2m_endpoint_add_humidity(en2m_endpoint_t *ep, const en2m_humidity_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL && driver->get_measured_value != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bad args");
    ep->clusters.present = true;
    ep->clusters.humidity = *driver;
    return ESP_OK;
}

esp_err_t en2m_endpoint_add_electrical_power(en2m_endpoint_t *ep, const en2m_electrical_power_driver_t *driver)
{
    ESP_RETURN_ON_FALSE(ep != NULL && driver != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ep->clusters.present = true;
    ep->clusters.electrical = *driver;
    return ESP_OK;
}

static void en2m_model_append_caps(cJSON *caps, const char *name)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, caps)
    {
        if (cJSON_IsString(item) && strcmp(item->valuestring, name) == 0) {
            return;
        }
    }
    cJSON_AddItemToArray(caps, cJSON_CreateString(name));
}

static cJSON *en2m_model_build_report_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *eps = cJSON_AddArrayToObject(root, "endpoints");
    cJSON *caps = cJSON_AddArrayToObject(root, "caps");
    const char *role = "leaf";

    if (en2m_get_role() == EN2M_ROLE_ROUTER) {
        role = "router";
    } else if (en2m_get_role() == EN2M_ROLE_COORDINATOR) {
        role = "coordinator";
    }
    cJSON_AddStringToObject(root, "node_role", role);
    cJSON_AddStringToObject(root, "interaction", "en2m_model");

    if (en2m_has_parent()) {
        uint8_t mac[6];
        char mac_str[18];
        en2m_get_parent_mac(mac);
        en2m_mac_to_str(mac, mac_str);
        cJSON_AddNumberToObject(root, "path_cost", en2m_get_path_cost());
        cJSON_AddStringToObject(root, "parent", mac_str);
    }

    for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
        en2m_endpoint_t *ep = &s_model.endpoints[i];
        cJSON *ep_obj;
        cJSON *clusters;

        if (!ep->used) {
            continue;
        }

        ep_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(ep_obj, "id", ep->id);
        clusters = cJSON_AddObjectToObject(ep_obj, "clusters");

        if (ep->clusters.on_off.get != NULL) {
            bool on = false;
            cJSON *obj = cJSON_CreateObject();
            if (ep->clusters.on_off.get(&on, ep->clusters.on_off.ctx) == ESP_OK) {
                cJSON_AddBoolToObject(obj, "on_off", on);
                /* HA-facing flat aliases */
                cJSON_AddStringToObject(root, "switch", on ? "ON" : "OFF");
            }
            cJSON_AddItemToObject(clusters, "on_off", obj);
            en2m_model_append_caps(caps, "switch");
        }

        if (ep->clusters.level.get_level != NULL) {
            uint8_t level = 0;
            cJSON *obj = cJSON_CreateObject();
            if (ep->clusters.level.get_level(&level, ep->clusters.level.ctx) == ESP_OK) {
                cJSON_AddNumberToObject(obj, "current_level", level);
                cJSON_AddNumberToObject(root, "level", level);
            }
            cJSON_AddItemToObject(clusters, "level_control", obj);
            en2m_model_append_caps(caps, "level");
        }

        if (ep->clusters.boolean_state.get != NULL) {
            bool value = false;
            cJSON *obj = cJSON_CreateObject();
            if (ep->clusters.boolean_state.get(&value, ep->clusters.boolean_state.ctx) == ESP_OK) {
                cJSON_AddBoolToObject(obj, "state_value", value);
                cJSON_AddStringToObject(root, "contact", value ? "ON" : "OFF");
            }
            cJSON_AddItemToObject(clusters, "boolean_state", obj);
            en2m_model_append_caps(caps, "contact");
        }

        if (ep->clusters.temperature.get_measured_value != NULL) {
            int16_t centi = 0;
            cJSON *obj = cJSON_CreateObject();
            if (ep->clusters.temperature.get_measured_value(&centi, ep->clusters.temperature.ctx) == ESP_OK) {
                double deg = centi / 100.0;
                cJSON_AddNumberToObject(obj, "measured_value", centi);
                cJSON_AddNumberToObject(root, "temperature", deg);
            }
            cJSON_AddItemToObject(clusters, "temperature_measurement", obj);
            en2m_model_append_caps(caps, "temperature");
        }

        if (ep->clusters.humidity.get_measured_value != NULL) {
            uint16_t centi = 0;
            cJSON *obj = cJSON_CreateObject();
            if (ep->clusters.humidity.get_measured_value(&centi, ep->clusters.humidity.ctx) == ESP_OK) {
                double pct = centi / 100.0;
                cJSON_AddNumberToObject(obj, "measured_value", centi);
                cJSON_AddNumberToObject(root, "humidity", pct);
            }
            cJSON_AddItemToObject(clusters, "relative_humidity_measurement", obj);
            en2m_model_append_caps(caps, "humidity");
        }

        if (ep->clusters.electrical.get_active_power != NULL || ep->clusters.electrical.get_energy != NULL) {
            cJSON *obj = cJSON_CreateObject();
            if (ep->clusters.electrical.get_active_power != NULL) {
                int32_t mw = 0;
                if (ep->clusters.electrical.get_active_power(&mw, ep->clusters.electrical.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(obj, "active_power", mw);
                    cJSON_AddNumberToObject(root, "power", mw / 1000.0);
                    en2m_model_append_caps(caps, "power");
                }
            }
            if (ep->clusters.electrical.get_energy != NULL) {
                int64_t mwh = 0;
                if (ep->clusters.electrical.get_energy(&mwh, ep->clusters.electrical.ctx) == ESP_OK) {
                    cJSON_AddNumberToObject(obj, "energy", (double)mwh);
                    cJSON_AddNumberToObject(root, "energy", mwh / 1000.0);
                    en2m_model_append_caps(caps, "energy");
                }
            }
            cJSON_AddItemToObject(clusters, "electrical_power", obj);
        }

        cJSON_AddItemToArray(eps, ep_obj);
    }

    return root;
}

esp_err_t en2m_model_report(void)
{
    cJSON *root;
    char *printed;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(s_model.started, ESP_ERR_INVALID_STATE, TAG, "model not started");

    root = en2m_model_build_report_json();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "json alloc failed");

    printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed != NULL, ESP_ERR_NO_MEM, TAG, "json print failed");

    err = en2m_send_uplink(EN2M_MSG_STATE, 0, (const uint8_t *)printed,
                           (uint8_t)strnlen(printed, EN2M_DATA_MAX));
    cJSON_free(printed);
    s_model.last_report_us = en2m_model_now_us();
    return err;
}

static esp_err_t en2m_model_handle_command(const en2m_pkt_t *pkt, void *user_ctx)
{
    char tmp[EN2M_DATA_MAX + 1] = {0};
    cJSON *doc;
    const cJSON *ep_j;
    const cJSON *cluster_j;
    const cJSON *cmd_j;
    en2m_endpoint_t *ep;
    uint8_t ep_id = 1;

    (void)user_ctx;
    if (pkt->data_len > 0) {
        memcpy(tmp, pkt->data, pkt->data_len);
    }

    doc = cJSON_Parse(tmp);
    if (doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ep_j = cJSON_GetObjectItem(doc, "ep");
    if (cJSON_IsNumber(ep_j)) {
        ep_id = (uint8_t)ep_j->valueint;
    }

    ep = en2m_endpoint_find(ep_id);
    if (ep == NULL) {
        /* Legacy flat command: {"switch":"ON"} */
        const cJSON *sw = cJSON_GetObjectItem(doc, "switch");
        if (cJSON_IsString(sw)) {
            for (int i = 0; i < EN2M_MAX_ENDPOINTS; i++) {
                if (s_model.endpoints[i].used && s_model.endpoints[i].clusters.on_off.set != NULL) {
                    ep = &s_model.endpoints[i];
                    break;
                }
            }
            if (ep != NULL && ep->clusters.on_off.set != NULL) {
                bool on = (strcmp(sw->valuestring, "ON") == 0);
                if (strcmp(sw->valuestring, "TOGGLE") == 0) {
                    bool cur = false;
                    if (ep->clusters.on_off.get) {
                        ep->clusters.on_off.get(&cur, ep->clusters.on_off.ctx);
                    }
                    on = !cur;
                }
                ep->clusters.on_off.set(on, ep->clusters.on_off.ctx);
                cJSON_Delete(doc);
                en2m_model_report();
                return ESP_OK;
            }
        }
        cJSON_Delete(doc);
        return ESP_ERR_NOT_FOUND;
    }

    cluster_j = cJSON_GetObjectItem(doc, "cluster");
    cmd_j = cJSON_GetObjectItem(doc, "command");
    if (!cJSON_IsString(cluster_j) || !cJSON_IsString(cmd_j)) {
        cJSON_Delete(doc);
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(cluster_j->valuestring, "on_off") == 0 && ep->clusters.on_off.set != NULL) {
        bool on = false;
        bool cur = false;
        if (strcmp(cmd_j->valuestring, "on") == 0) {
            on = true;
        } else if (strcmp(cmd_j->valuestring, "off") == 0) {
            on = false;
        } else if (strcmp(cmd_j->valuestring, "toggle") == 0) {
            if (ep->clusters.on_off.get) {
                ep->clusters.on_off.get(&cur, ep->clusters.on_off.ctx);
            }
            on = !cur;
        }
        ep->clusters.on_off.set(on, ep->clusters.on_off.ctx);
    } else if (strcmp(cluster_j->valuestring, "level_control") == 0 && ep->clusters.level.set_level != NULL) {
        const cJSON *level_j = cJSON_GetObjectItem(doc, "level");
        if (cJSON_IsNumber(level_j)) {
            ep->clusters.level.set_level((uint8_t)level_j->valueint, ep->clusters.level.ctx);
        }
    }

    cJSON_Delete(doc);
    en2m_model_report();
    return ESP_OK;
}

static void en2m_model_on_command(const en2m_pkt_t *pkt, void *user_ctx)
{
    en2m_model_handle_command(pkt, user_ctx);
}

esp_err_t en2m_model_start(const en2m_config_t *mesh_config)
{
    en2m_config_t cfg;

    ESP_RETURN_ON_FALSE(mesh_config != NULL, ESP_ERR_INVALID_ARG, TAG, "mesh_config NULL");

    cfg = *mesh_config;
    if (cfg.on_command == NULL) {
        cfg.on_command = en2m_model_on_command;
    }

    ESP_RETURN_ON_ERROR(en2m_mesh_init(&cfg), TAG, "mesh init failed");
    s_model.started = true;
    s_model.report_interval_ms = (cfg.role == EN2M_ROLE_LEAF) ? 30000 : 15000;
    en2m_model_report();
    return ESP_OK;
}

void en2m_model_loop(void)
{
    en2m_mesh_loop();
    if (!s_model.started) {
        return;
    }
    if ((en2m_model_now_us() - s_model.last_report_us) / 1000 >= s_model.report_interval_ms) {
        en2m_model_report();
    }
}

esp_err_t en2m_model_notify(uint8_t endpoint_id, en2m_cluster_id_t cluster, bool immediate)
{
    (void)endpoint_id;
    (void)cluster;
    if (immediate) {
        return en2m_model_report();
    }
    s_model.last_report_us = 0; /* force soon */
    return ESP_OK;
}
