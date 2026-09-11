/**
 * @file en2m_model.h
 * @brief Interaction / data-model layer (ESP-Matter style).
 *
 * This layer owns endpoints, clusters, attributes and commands.
 * It does **not** own hardware drivers — the application binds drivers
 * through ops callbacks (similar to ESP-Matter's driver glue).
 *
 * Layers:
 *   Application + drivers  →  en2m model (clusters)  →  en2m mesh (ESP-NOW)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "en2m_mesh.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Matter-inspired cluster identifiers (subset). */
typedef enum {
    EN2M_CLUSTER_ON_OFF = 0x0006,
    EN2M_CLUSTER_LEVEL_CONTROL = 0x0008,
    EN2M_CLUSTER_BOOLEAN_STATE = 0x0045,
    EN2M_CLUSTER_TEMPERATURE_MEASUREMENT = 0x0402,
    EN2M_CLUSTER_RELATIVE_HUMIDITY = 0x0405,
    EN2M_CLUSTER_ELECTRICAL_POWER = 0x0B04,
} en2m_cluster_id_t;

#define EN2M_MAX_ENDPOINTS 4

typedef struct en2m_endpoint en2m_endpoint_t;

/* ---- Driver ops (interaction ← application) ---- */

typedef struct {
    esp_err_t (*set)(bool on, void *ctx);
    esp_err_t (*get)(bool *on, void *ctx);
    void *ctx;
} en2m_on_off_driver_t;

typedef struct {
    /** level 0–254 (Matter-like); 255 = null / unused */
    esp_err_t (*set_level)(uint8_t level, void *ctx);
    esp_err_t (*get_level)(uint8_t *level, void *ctx);
    void *ctx;
} en2m_level_driver_t;

typedef struct {
    esp_err_t (*get)(bool *state_value, void *ctx);
    void *ctx;
} en2m_boolean_state_driver_t;

typedef struct {
    /** Return temperature in 0.01 °C (e.g. 2350 = 23.50 °C), Matter-style. */
    esp_err_t (*get_measured_value)(int16_t *centi_celsius, void *ctx);
    void *ctx;
} en2m_temperature_driver_t;

typedef struct {
    /** Return humidity in 0.01 % (e.g. 5510 = 55.10 %). */
    esp_err_t (*get_measured_value)(uint16_t *centi_percent, void *ctx);
    void *ctx;
} en2m_humidity_driver_t;

typedef struct {
    esp_err_t (*get_active_power)(int32_t *milliwatts, void *ctx);
    esp_err_t (*get_energy)(int64_t *milliwatt_hours, void *ctx);
    void *ctx;
} en2m_electrical_power_driver_t;

/**
 * @brief Create an endpoint (1..254). Call before ::en2m_model_start.
 */
en2m_endpoint_t *en2m_endpoint_create(uint8_t endpoint_id);

esp_err_t en2m_endpoint_add_on_off(en2m_endpoint_t *ep, const en2m_on_off_driver_t *driver);
esp_err_t en2m_endpoint_add_level_control(en2m_endpoint_t *ep, const en2m_level_driver_t *driver);
esp_err_t en2m_endpoint_add_boolean_state(en2m_endpoint_t *ep, const en2m_boolean_state_driver_t *driver);
esp_err_t en2m_endpoint_add_temperature(en2m_endpoint_t *ep, const en2m_temperature_driver_t *driver);
esp_err_t en2m_endpoint_add_humidity(en2m_endpoint_t *ep, const en2m_humidity_driver_t *driver);
esp_err_t en2m_endpoint_add_electrical_power(en2m_endpoint_t *ep, const en2m_electrical_power_driver_t *driver);

/**
 * @brief Init mesh transport and mark the data model ready.
 *
 * @note Does not touch GPIO / I2C / etc. Drivers must already be bound.
 */
esp_err_t en2m_model_start(const en2m_config_t *mesh_config);

/** Periodic: mesh maintenance + optional auto-report. */
void en2m_model_loop(void);

/** Force an attribute report uplink now. */
esp_err_t en2m_model_report(void);

/**
 * @brief Notify model that a measured attribute changed (e.g. contact toggled).
 * Triggers a report on next loop or immediately if @p immediate.
 */
esp_err_t en2m_model_notify(uint8_t endpoint_id, en2m_cluster_id_t cluster, bool immediate);

#ifdef __cplusplus
}
#endif
