#include "en2m_mesh.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

static const char *TAG = "en2m_mesh";

typedef struct {
    bool used;
    uint8_t mac[6];
    uint8_t role;
    uint8_t cost;
    int8_t rssi;
    int64_t last_us;
} neighbor_t;

typedef struct {
    bool used;
    uint8_t dest[6];
    uint8_t next_hop[6];
    uint8_t hop;
    int64_t last_us;
} route_t;

static struct {
    bool inited;
    en2m_app_config_t cfg;
    char name[16];
    uint8_t self_mac[6];
    uint32_t seq;
    bool pairing;
    bool has_parent;
    uint8_t parent_mac[6];
    uint8_t path_cost;
    int64_t parent_last_us;
    neighbor_t neighbors[EN2M_MAX_NEIGHBORS];
    route_t routes[EN2M_MAX_ROUTES];
    int64_t last_beacon_us;
    int64_t last_hello_us;
    SemaphoreHandle_t lock;
} g;

static int64_t now_us(void) { return esp_timer_get_time(); }
static int64_t now_ms(void) { return now_us() / 1000; }

void en2m_mac_to_str(const uint8_t mac[6], char buf[18])
{
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool en2m_mac_from_str(const char *s, uint8_t out[6])
{
    unsigned b[6];
    if (sscanf(s, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[i];
    }
    return true;
}

static void log_msg(const char *msg)
{
    if (g.cfg.on_log) {
        g.cfg.on_log(msg, g.cfg.user);
    } else {
        ESP_LOGI(TAG, "%s", msg);
    }
}

static bool add_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }
    esp_now_peer_info_t peer = {0};
    en2m_mac_copy(peer.peer_addr, mac);
    peer.channel = g.cfg.channel ? g.cfg.channel : EN2M_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

static void fill_identity(en2m_pkt_t *pkt)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->magic = EN2M_MAGIC;
    pkt->version = EN2M_VERSION;
    pkt->role = g.cfg.role;
    pkt->hop_limit = EN2M_HOP_LIMIT;
    pkt->seq = g.seq++;
    const char *model = g.cfg.model ? g.cfg.model : EN2M_DEVICE_MODEL;
    strncpy(pkt->model, model, sizeof(pkt->model) - 1);
    strncpy(pkt->name, g.name, sizeof(pkt->name) - 1);
}

static esp_err_t send_raw(const uint8_t to[6], en2m_pkt_t *pkt)
{
    if (!add_peer(to)) {
        return ESP_FAIL;
    }
    return esp_now_send(to, (uint8_t *)pkt, sizeof(*pkt));
}

static void remember_neighbor(const uint8_t mac[6], uint8_t role, uint8_t cost, int8_t rssi)
{
    int free_idx = -1;
    for (int i = 0; i < EN2M_MAX_NEIGHBORS; i++) {
        if (g.neighbors[i].used && en2m_mac_equal(g.neighbors[i].mac, mac)) {
            g.neighbors[i].role = role;
            g.neighbors[i].cost = cost;
            g.neighbors[i].rssi = rssi;
            g.neighbors[i].last_us = now_us();
            return;
        }
        if (!g.neighbors[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        return;
    }
    g.neighbors[free_idx].used = true;
    en2m_mac_copy(g.neighbors[free_idx].mac, mac);
    g.neighbors[free_idx].role = role;
    g.neighbors[free_idx].cost = cost;
    g.neighbors[free_idx].rssi = rssi;
    g.neighbors[free_idx].last_us = now_us();
}

static void learn_route(const uint8_t origin[6], const uint8_t from[6], uint8_t hop)
{
    if (en2m_mac_equal(origin, g.self_mac)) {
        return;
    }
    int free_idx = -1;
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (g.routes[i].used && en2m_mac_equal(g.routes[i].dest, origin)) {
            if (hop <= g.routes[i].hop) {
                en2m_mac_copy(g.routes[i].next_hop, from);
                g.routes[i].hop = hop;
            }
            g.routes[i].last_us = now_us();
            return;
        }
        if (!g.routes[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        return;
    }
    g.routes[free_idx].used = true;
    en2m_mac_copy(g.routes[free_idx].dest, origin);
    en2m_mac_copy(g.routes[free_idx].next_hop, from);
    g.routes[free_idx].hop = hop;
    g.routes[free_idx].last_us = now_us();
}

bool en2m_lookup_route(const uint8_t dest[6], uint8_t next_hop[6])
{
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (g.routes[i].used && en2m_mac_equal(g.routes[i].dest, dest)) {
            en2m_mac_copy(next_hop, g.routes[i].next_hop);
            return true;
        }
    }
    return false;
}

void en2m_forget_route(const uint8_t dest[6])
{
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (g.routes[i].used && en2m_mac_equal(g.routes[i].dest, dest)) {
            g.routes[i].used = false;
        }
    }
}

void en2m_clear_routes(void) { memset(g.routes, 0, sizeof(g.routes)); }

static void expire_routes(void)
{
    int64_t t = now_ms();
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (g.routes[i].used && (t - g.routes[i].last_us / 1000) > EN2M_ROUTE_STALE_MS) {
            g.routes[i].used = false;
        }
    }
    for (int i = 0; i < EN2M_MAX_NEIGHBORS; i++) {
        if (g.neighbors[i].used &&
            (t - g.neighbors[i].last_us / 1000) > (EN2M_PARENT_STALE_MS * 2)) {
            g.neighbors[i].used = false;
        }
    }
}

static void expire_parent(void)
{
    if (g.cfg.role == EN2M_ROLE_COORDINATOR) {
        return;
    }
    if (!g.has_parent) {
        return;
    }
    if ((now_ms() - g.parent_last_us / 1000) > EN2M_PARENT_STALE_MS) {
        g.has_parent = false;
        g.path_cost = 255;
        log_msg("parent stale");
    }
}

static void consider_parent(const uint8_t mac[6], uint8_t role, uint8_t their_cost, int8_t rssi)
{
    if (g.cfg.role == EN2M_ROLE_COORDINATOR) {
        return;
    }
    if (role != EN2M_ROLE_COORDINATOR && role != EN2M_ROLE_ROUTER) {
        return;
    }
    if (their_cost >= 254) {
        return;
    }
    uint8_t new_cost = (uint8_t)(their_cost + 1);
    if (new_cost > EN2M_HOP_LIMIT) {
        return;
    }

    bool better = false;
    if (!g.has_parent) {
        better = true;
    } else if (new_cost < g.path_cost) {
        better = true;
    } else if (new_cost == g.path_cost && en2m_mac_equal(g.parent_mac, mac)) {
        g.parent_last_us = now_us();
        return;
    } else if (new_cost == g.path_cost) {
        int8_t cur_rssi = -100;
        for (int i = 0; i < EN2M_MAX_NEIGHBORS; i++) {
            if (g.neighbors[i].used && en2m_mac_equal(g.neighbors[i].mac, g.parent_mac)) {
                cur_rssi = g.neighbors[i].rssi;
                break;
            }
        }
        if (rssi > cur_rssi + 8) {
            better = true;
        }
    }

    if (!better) {
        return;
    }
    en2m_mac_copy(g.parent_mac, mac);
    g.has_parent = true;
    g.path_cost = new_cost;
    g.parent_last_us = now_us();
    add_peer(g.parent_mac);

    char buf[64];
    char macs[18];
    en2m_mac_to_str(g.parent_mac, macs);
    snprintf(buf, sizeof(buf), "parent=%s cost=%u rssi=%d", macs, g.path_cost, (int)rssi);
    log_msg(buf);
}

static void send_beacon(void)
{
    if (g.cfg.role == EN2M_ROLE_LEAF) {
        return;
    }
    uint8_t cost = 0;
    if (g.cfg.role == EN2M_ROLE_ROUTER) {
        if (!g.has_parent) {
            return;
        }
        cost = g.path_cost;
    }

    en2m_pkt_t pkt;
    fill_identity(&pkt);
    pkt.msg_type = EN2M_MSG_BEACON;
    pkt.cost = cost;
    pkt.hop = 0;
    if (g.pairing) {
        pkt.flags |= EN2M_FLAG_PAIRING;
    }
    en2m_mac_copy(pkt.origin, g.self_mac);
    en2m_mac_broadcast(pkt.dest);
    en2m_mac_copy(pkt.via, g.self_mac);
    uint8_t bcast[6];
    en2m_mac_broadcast(bcast);
    send_raw(bcast, &pkt);
}

static void forward_toward_coord(en2m_pkt_t *pkt)
{
    if (g.cfg.role != EN2M_ROLE_ROUTER || !g.has_parent) {
        return;
    }
    if (pkt->hop >= pkt->hop_limit) {
        return;
    }
    pkt->hop++;
    en2m_mac_copy(pkt->via, g.self_mac);
    send_raw(g.parent_mac, pkt);
}

static void forward_toward_dest(en2m_pkt_t *pkt)
{
    if (g.cfg.role == EN2M_ROLE_LEAF) {
        return;
    }
    if (pkt->hop >= pkt->hop_limit) {
        return;
    }
    uint8_t next[6];
    if (!en2m_lookup_route(pkt->dest, next)) {
        en2m_mac_copy(next, pkt->dest);
    }
    pkt->hop++;
    en2m_mac_copy(pkt->via, g.self_mac);
    send_raw(next, pkt);
}

static void handle_rx(const uint8_t from[6], const en2m_pkt_t *in, int8_t rssi)
{
    if (in->magic != EN2M_MAGIC || in->version != EN2M_VERSION) {
        return;
    }
    if (en2m_mac_equal(from, g.self_mac)) {
        return;
    }

    remember_neighbor(from, in->role, in->cost, rssi);

    if (in->msg_type == EN2M_MSG_BEACON) {
        consider_parent(from, in->role, in->cost, rssi);
        return;
    }

    if (in->msg_type == EN2M_MSG_CMD && en2m_mac_equal(in->dest, g.self_mac)) {
        if (g.cfg.on_command) {
            g.cfg.on_command(in, g.cfg.user);
        }
        return;
    }

    if (g.cfg.role == EN2M_ROLE_ROUTER && in->msg_type == EN2M_MSG_CMD &&
        !en2m_mac_equal(in->dest, g.self_mac)) {
        learn_route(in->origin, from, in->hop);
        en2m_pkt_t pkt = *in;
        forward_toward_dest(&pkt);
        return;
    }

    if (g.cfg.role == EN2M_ROLE_COORDINATOR) {
        bool uplink = (in->msg_type == EN2M_MSG_HELLO || in->msg_type == EN2M_MSG_STATE ||
                       in->msg_type == EN2M_MSG_ACK || in->msg_type == EN2M_MSG_HEARTBEAT);
        if (!uplink) {
            return;
        }
        uint8_t existing_next[6];
        bool known = en2m_lookup_route(in->origin, existing_next);
        if (!known && !g.pairing) {
            log_msg("drop uplink (not pairing / unknown)");
            return;
        }
        learn_route(in->origin, from, in->hop);
        if (g.cfg.on_uplink) {
            g.cfg.on_uplink(in, rssi, from, g.cfg.user);
        }
        return;
    }

    if (g.cfg.role == EN2M_ROLE_ROUTER &&
        (in->msg_type == EN2M_MSG_HELLO || in->msg_type == EN2M_MSG_STATE ||
         in->msg_type == EN2M_MSG_ACK || in->msg_type == EN2M_MSG_HEARTBEAT)) {
        learn_route(in->origin, from, in->hop);
        en2m_pkt_t pkt = *in;
        forward_toward_coord(&pkt);
    }
}

static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!g.inited || !info || !data || len < (int)offsetof(en2m_pkt_t, data)) {
        return;
    }
    int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    if (g.lock) {
        xSemaphoreTake(g.lock, portMAX_DELAY);
    }
    handle_rx(info->src_addr, (const en2m_pkt_t *)data, rssi);
    if (g.lock) {
        xSemaphoreGive(g.lock);
    }
}

static esp_err_t wifi_init_for_espnow(uint8_t channel)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    return ESP_OK;
}

esp_err_t en2m_mesh_init(const en2m_app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->role != EN2M_ROLE_COORDINATOR && cfg->role != EN2M_ROLE_ROUTER &&
        cfg->role != EN2M_ROLE_LEAF) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&g, 0, sizeof(g));
    g.cfg = *cfg;
    if (!g.cfg.channel) {
        g.cfg.channel = EN2M_WIFI_CHANNEL;
    }
    strncpy(g.name, cfg->name ? cfg->name : EN2M_DEVICE_NAME, sizeof(g.name) - 1);
    g.path_cost = 255;
    g.lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    esp_err_t net = esp_netif_init();
    if (net != ESP_OK && net != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(net);
    }
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ev);
    }
    ESP_ERROR_CHECK(wifi_init_for_espnow(g.cfg.channel));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, g.self_mac));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_espnow_recv));

    uint8_t bcast[6];
    en2m_mac_broadcast(bcast);
    add_peer(bcast);

    if (g.cfg.role == EN2M_ROLE_COORDINATOR) {
        g.path_cost = 0;
        g.has_parent = true;
    }

    g.inited = true;
    log_msg("mesh init (esp-idf)");
    return ESP_OK;
}

void en2m_mesh_deinit(void)
{
    if (!g.inited) {
        return;
    }
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    g.inited = false;
}

void en2m_mesh_loop(void)
{
    if (!g.inited) {
        return;
    }
    if (g.lock) {
        xSemaphoreTake(g.lock, portMAX_DELAY);
    }
    expire_parent();
    expire_routes();

    if (g.cfg.role != EN2M_ROLE_LEAF) {
        if ((now_ms() - g.last_beacon_us / 1000) >= EN2M_BEACON_MS_DEFAULT) {
            g.last_beacon_us = now_us();
            send_beacon();
        }
    }

    if (g.cfg.role != EN2M_ROLE_COORDINATOR) {
        if ((now_ms() - g.last_hello_us / 1000) > 30000) {
            g.last_hello_us = now_us();
            en2m_send_uplink(EN2M_MSG_HEARTBEAT, 0, NULL, 0);
        }
    }
    if (g.lock) {
        xSemaphoreGive(g.lock);
    }
}

esp_err_t en2m_send_uplink(uint8_t msg_type, uint16_t cmd_id,
                           const uint8_t *data, uint8_t len)
{
    if (g.cfg.role == EN2M_ROLE_COORDINATOR) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!g.has_parent && msg_type != EN2M_MSG_HELLO && msg_type != EN2M_MSG_HEARTBEAT) {
        return ESP_ERR_INVALID_STATE;
    }
    en2m_pkt_t pkt;
    fill_identity(&pkt);
    pkt.msg_type = msg_type;
    pkt.cmd_id = cmd_id;
    pkt.hop = 0;
    pkt.cost = g.path_cost;
    en2m_mac_copy(pkt.origin, g.self_mac);
    en2m_mac_broadcast(pkt.dest);
    en2m_mac_copy(pkt.via, g.self_mac);
    if (data && len) {
        if (len > EN2M_DATA_MAX) {
            len = EN2M_DATA_MAX;
        }
        pkt.data_len = len;
        memcpy(pkt.data, data, len);
    }
    if (g.has_parent) {
        return send_raw(g.parent_mac, &pkt);
    }
    uint8_t bcast[6];
    en2m_mac_broadcast(bcast);
    return send_raw(bcast, &pkt);
}

esp_err_t en2m_send_downlink(const uint8_t dest_mac[6], uint16_t cmd_id,
                             const uint8_t *data, uint8_t len)
{
    if (g.cfg.role != EN2M_ROLE_COORDINATOR) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    en2m_pkt_t pkt;
    fill_identity(&pkt);
    pkt.msg_type = EN2M_MSG_CMD;
    pkt.cmd_id = cmd_id;
    pkt.hop = 0;
    pkt.cost = 0;
    en2m_mac_copy(pkt.origin, g.self_mac);
    en2m_mac_copy(pkt.dest, dest_mac);
    en2m_mac_copy(pkt.via, g.self_mac);
    if (data && len) {
        if (len > EN2M_DATA_MAX) {
            len = EN2M_DATA_MAX;
        }
        pkt.data_len = len;
        memcpy(pkt.data, data, len);
    }
    uint8_t next[6];
    if (en2m_lookup_route(dest_mac, next)) {
        return send_raw(next, &pkt);
    }
    return send_raw(dest_mac, &pkt);
}

void en2m_set_pairing(bool enabled) { g.pairing = enabled; }
bool en2m_pairing(void) { return g.pairing; }
bool en2m_has_parent(void) { return g.has_parent; }
uint8_t en2m_path_cost(void) { return g.path_cost; }
uint8_t en2m_role(void) { return g.cfg.role; }

void en2m_parent_mac(uint8_t out[6])
{
    if (g.has_parent) {
        en2m_mac_copy(out, g.parent_mac);
    } else {
        en2m_mac_broadcast(out);
    }
}

void en2m_self_mac(uint8_t out[6]) { en2m_mac_copy(out, g.self_mac); }

void en2m_set_name(const char *name)
{
    if (!name) {
        return;
    }
    strncpy(g.name, name, sizeof(g.name) - 1);
    g.name[sizeof(g.name) - 1] = 0;
}
