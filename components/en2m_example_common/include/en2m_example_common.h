/**
 * @file en2m_example_common.h
 * @brief Shared helpers for firmware examples (not part of core mesh API).
 */

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
    en2m_role_t role;             /**< Usually EN2M_ROLE_LEAF */
    const char *name;             /**< Friendly name / MQTT slug */
    const char *model;            /**< Model string */
    const char *fw;               /**< Optional firmware version */
    uint8_t channel;              /**< 0 = default */
    en2m_command_cb_t on_command; /**< NULL for report-only devices */
    void *user_ctx;
} en2m_example_config_t;

/** @deprecated Prefer ::en2m_example_config_t */
typedef en2m_example_config_t en2m_example_cfg_t;

esp_err_t en2m_example_mesh_start(const en2m_example_config_t *config);
void en2m_example_mesh_tick(void);

esp_err_t en2m_example_send_hello(const char *json_object);
esp_err_t en2m_example_send_state(const char *json_object);
esp_err_t en2m_example_send_ack(uint16_t cmd_id, const char *json_object);

/**
 * @brief Build a compact JSON object into @p buf.
 *
 * @param caps_csv Comma-separated capability list, e.g. "temperature,humidity"
 * @param extras   Optional extra JSON fields without surrounding braces
 * @return Length written, or -1 on overflow / error
 */
int en2m_example_build_base_json(char *buf, size_t buflen, const char *caps_csv, const char *extras);

bool en2m_example_dht_read(int gpio_num, int dht_type, float *temp_c, float *humidity);

#ifdef __cplusplus
}
#endif
