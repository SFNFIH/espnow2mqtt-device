/**
 * Door lock — DoorLock cluster.
 *
 * The lock state is persisted, so en2m_start replays it into the actuator
 * before the first report goes out.
 */
#include "en2m.h"
#include "esp_log.h"

#define ENDPOINT 1

static const char *TAG = "ex_lock";

static esp_err_t bolt_drive(bool locked)
{
    /* Replace with the solenoid / motor sequence. */
    ESP_LOGI(TAG, "bolt %s", locked ? "extended" : "retracted");
    return ESP_OK;
}

static esp_err_t on_write(const en2m_attr_path_t *path, const en2m_value_t *value, void *ctx)
{
    (void)ctx;

    if (path->cluster_id != EN2M_CLUSTER_DOOR_LOCK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return bolt_drive(value->v.e8 == EN2M_LOCK_LOCKED);
}

void app_main(void)
{
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "lock1", .model = "ex-lock"},
        .attribute_write = on_write,
    };

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_DOOR_LOCK) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready");
}
