/**
 * Deprecated combo leaf — prefer examples/*.
 * Kept as a quick "all-in-one" demo (TH + switch).
 * For production examples see:
 *   examples/th_sensor, contact_sensor, relay_switch, smart_plug
 */
#include "en2m_device_app.h"
#include "en2m_config.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGW("leaf", "combo leaf demo; prefer examples/");
    ESP_ERROR_CHECK(en2m_device_app_start(EN2M_ROLE_LEAF, "leaf1", "c3-leaf"));
}
