/**
 * Mains-powered mesh router — transport only, no device clusters.
 *
 * A router forwards frames in both directions and rebroadcasts beacons so
 * leaves further out can attach. All of that happens on the component's own
 * task, so app_main has nothing left to do but report what it sees.
 */
#include "en2m.h"
#include "esp_log.h"

static const char *TAG = "router";

static void on_en2m_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;

    switch (event_id) {
    case EN2M_EVENT_PARENT_FOUND: {
        const en2m_event_parent_t *parent = data;
        char mac[18];
        en2m_mac_to_str(parent->mac, mac);
        ESP_LOGI(TAG, "attached to %s at cost %u (rssi %d)", mac, parent->cost, parent->rssi);
        break;
    }
    case EN2M_EVENT_PARENT_LOST:
        ESP_LOGW(TAG, "lost the uplink, looking for a new parent");
        break;
    case EN2M_EVENT_RX_DROPPED: {
        const en2m_event_dropped_t *dropped = data;
        ESP_LOGW(TAG, "%u frames dropped, consider raising EN2M_QUEUE_LEN", (unsigned)dropped->total);
        break;
    }
    default:
        break;
    }
}

void app_main(void)
{
    en2m_config_t cfg = {
        .role = EN2M_ROLE_ROUTER,
        .name = "router1",
        .model = "ex-router",
        .fw = EN2M_FW_VERSION,
    };

    ESP_ERROR_CHECK(en2m_event_handler_register(EN2M_EVENT_ANY, on_en2m_event, NULL));
    ESP_ERROR_CHECK(en2m_mesh_init(&cfg));
    ESP_LOGI(TAG, "router up — forwarding runs on the en2m task");
}
