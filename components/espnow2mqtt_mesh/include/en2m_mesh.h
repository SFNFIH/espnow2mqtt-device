#pragma once

#include "en2m_proto.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*en2m_uplink_cb_t)(const en2m_pkt_t *pkt, int8_t rssi,
                                 const uint8_t from_mac[6], void *user);
typedef void (*en2m_command_cb_t)(const en2m_pkt_t *pkt, void *user);
typedef void (*en2m_log_cb_t)(const char *msg, void *user);

typedef struct {
    uint8_t role; /* EN2M_ROLE_COORDINATOR / ROUTER / LEAF */
    const char *model;
    const char *name;
    const char *fw;
    uint8_t channel;
    en2m_uplink_cb_t on_uplink;
    en2m_command_cb_t on_command;
    en2m_log_cb_t on_log;
    void *user;
} en2m_app_config_t;

esp_err_t en2m_mesh_init(const en2m_app_config_t *cfg);
void en2m_mesh_deinit(void);
void en2m_mesh_loop(void);

esp_err_t en2m_send_uplink(uint8_t msg_type, uint16_t cmd_id,
                           const uint8_t *data, uint8_t len);
esp_err_t en2m_send_downlink(const uint8_t dest_mac[6], uint16_t cmd_id,
                             const uint8_t *data, uint8_t len);

void en2m_set_pairing(bool enabled);
bool en2m_pairing(void);

bool en2m_has_parent(void);
uint8_t en2m_path_cost(void);
uint8_t en2m_role(void);
void en2m_parent_mac(uint8_t out[6]);
void en2m_self_mac(uint8_t out[6]);

bool en2m_lookup_route(const uint8_t dest[6], uint8_t next_hop[6]);
void en2m_forget_route(const uint8_t dest[6]);
void en2m_clear_routes(void);
void en2m_set_name(const char *name);

#ifdef __cplusplus
}
#endif
