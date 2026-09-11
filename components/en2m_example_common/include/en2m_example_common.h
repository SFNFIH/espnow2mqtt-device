#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "en2m_mesh.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t role; /* usually EN2M_ROLE_LEAF; routers use EN2M_ROLE_ROUTER */
    const char *name;
    const char *model;
    const char *fw;
    uint8_t channel;
    en2m_command_cb_t on_command; /* NULL for report-only devices */
    void *user;
} en2m_example_cfg_t;

esp_err_t en2m_example_mesh_start(const en2m_example_cfg_t *cfg);

/** Periodic mesh maintenance; call from app task. */
void en2m_example_mesh_tick(void);

/** Publish HELLO with optional JSON object string (already formatted object body). */
esp_err_t en2m_example_send_hello(const char *json_object);

/** Publish STATE JSON object. */
esp_err_t en2m_example_send_state(const char *json_object);

/** Publish ACK for a command id. */
esp_err_t en2m_example_send_ack(uint16_t cmd_id, const char *json_object);

/**
 * Build a JSON object string into buf.
 * extras is optional already-serialized fragment without braces, e.g.
 *   "\"temperature\":23.5,\"humidity\":40"
 * Always includes node_role / path_cost / parent when attached.
 * Returns length or -1.
 */
int en2m_example_build_base_json(char *buf, size_t buflen, const char *caps_csv,
                                 const char *extras);

bool en2m_example_dht_read(int gpio, int dht_type, float *temp_c, float *hum);

#ifdef __cplusplus
}
#endif
