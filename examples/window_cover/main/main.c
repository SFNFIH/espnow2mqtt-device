/**
 * Window covering — WindowCovering cluster.
 *
 * A motor does not reach its target instantly, so this example handles the
 * cluster with a command callback instead of a write callback: commands start
 * or stop the motor, and the travel timer publishes the position it actually
 * reached with en2m_report_cover_position(). Position 0 is fully open,
 * 100 is fully closed.
 */
#include "en2m.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#define ENDPOINT 1
#define STEP_PERIOD_US 200000
#define STEP_PERCENT 5

static const char *TAG = "ex_cover";

static struct {
    uint8_t position;
    uint8_t target;
    bool moving;
    esp_timer_handle_t timer;
} s_cover;

static void motor_stop(void)
{
    s_cover.moving = false;
    s_cover.target = s_cover.position;
    esp_timer_stop(s_cover.timer);
    en2m_report_cover_position(ENDPOINT, s_cover.position);
    ESP_LOGI(TAG, "stopped at %u%%", s_cover.position);
}

static void motor_step(void *arg)
{
    (void)arg;

    if (s_cover.position < s_cover.target) {
        uint8_t remaining = s_cover.target - s_cover.position;
        s_cover.position += (remaining < STEP_PERCENT) ? remaining : STEP_PERCENT;
    } else if (s_cover.position > s_cover.target) {
        uint8_t remaining = s_cover.position - s_cover.target;
        s_cover.position -= (remaining < STEP_PERCENT) ? remaining : STEP_PERCENT;
    }

    en2m_report_cover_position(ENDPOINT, s_cover.position);
    if (s_cover.position == s_cover.target) {
        motor_stop();
    }
}

static void motor_go(uint8_t target)
{
    s_cover.target = (target > 100) ? 100 : target;
    if (s_cover.target == s_cover.position) {
        en2m_report_cover_position(ENDPOINT, s_cover.position);
        return;
    }
    if (!s_cover.moving) {
        s_cover.moving = true;
        esp_timer_start_periodic(s_cover.timer, STEP_PERIOD_US);
    }
    ESP_LOGI(TAG, "moving %u%% -> %u%%", s_cover.position, s_cover.target);
}

static esp_err_t on_command(const en2m_command_t *command, void *ctx)
{
    (void)ctx;

    if (command->cluster_id != EN2M_CLUSTER_WINDOW_COVERING) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    switch (command->id) {
    case EN2M_CMD_UP_OR_OPEN:
        motor_go(0);
        return ESP_OK;
    case EN2M_CMD_DOWN_OR_CLOSE:
        motor_go(100);
        return ESP_OK;
    case EN2M_CMD_GO_TO_LIFT_PERCENTAGE:
        motor_go((uint8_t)en2m_value_as_int(&command->arg));
        return ESP_OK;
    case EN2M_CMD_STOP_MOTION:
        motor_stop();
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

void app_main(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = motor_step,
        .name = "cover_travel",
    };
    en2m_device_config_t cfg = {
        .mesh = {.role = EN2M_ROLE_LEAF, .name = "cover1", .model = "ex-cover"},
        .command = on_command,
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_cover.timer));

    if (en2m_endpoint_create_device(ENDPOINT, EN2M_DEVICE_TYPE_WINDOW_COVERING) == NULL) {
        ESP_LOGE(TAG, "could not create the endpoint");
        return;
    }

    ESP_ERROR_CHECK(en2m_start(&cfg));
    ESP_LOGI(TAG, "ready — travel is reported as it happens");
}
