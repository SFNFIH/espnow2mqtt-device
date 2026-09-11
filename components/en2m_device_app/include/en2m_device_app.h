#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @param role EN2M_ROLE_LEAF or EN2M_ROLE_ROUTER
 * @param name friendly name (MQTT slug), nullable → EN2M_DEVICE_NAME
 * @param model model string, nullable
 */
esp_err_t en2m_device_app_start(uint8_t role, const char *name, const char *model);

#ifdef __cplusplus
}
#endif
