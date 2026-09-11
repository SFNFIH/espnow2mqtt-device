/**
 * Gesture recognition for a momentary push button.
 *
 * The interrupt does nothing but debounce and hand the edge to a task, which
 * runs the state machine using the queue receive timeout as its only clock.
 * That keeps every decision in one place and off the interrupt, so the
 * application callback has no context restrictions.
 */

#include "drv_gpio_button_gesture.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DRV_BUTTON_QUEUE_LEN 8
#define DRV_BUTTON_TASK_STACK 2560
#define DRV_BUTTON_TASK_PRIO 5

/** What the state machine is waiting for. */
typedef enum {
    ST_IDLE,        /**< Button up, nothing pending */
    ST_PRESSED,     /**< Button down, long-press threshold not reached */
    ST_WAIT_DOUBLE, /**< One short press seen, waiting to see whether a second follows */
    ST_LONG_HELD,   /**< Long press already reported, waiting for the release */
    ST_CONSUMED,    /**< Double press already reported, ignore its release */
} drv_button_state_t;

typedef struct {
    bool pressed;
} drv_button_edge_t;

typedef struct {
    gpio_num_t pin;
    bool active_low;
    int64_t debounce_us;
    int64_t last_us;
    uint32_t long_ms;
    uint32_t double_gap_ms;
    drv_gpio_button_gesture_cb_t on_gesture;
    void *ctx;
    QueueHandle_t queue;
} drv_button_gesture_t;

static drv_button_gesture_t s_gesture;

static void IRAM_ATTR drv_button_gesture_isr(void *arg)
{
    drv_button_gesture_t *btn = (drv_button_gesture_t *)arg;
    int64_t now = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    drv_button_edge_t edge;

    if (now - btn->last_us < btn->debounce_us) {
        return;
    }
    btn->last_us = now;

    /* Read the level rather than tracking edge polarity: a bounce that was
     * filtered out above may have left the two out of step. */
    edge.pressed = (gpio_get_level(btn->pin) == 0) == btn->active_low;
    xQueueSendFromISR(btn->queue, &edge, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void drv_button_emit(drv_button_gesture_t *btn, drv_gpio_button_gesture_t gesture)
{
    if (btn->on_gesture != NULL) {
        btn->on_gesture(gesture, btn->ctx);
    }
}

static void drv_button_gesture_task(void *arg)
{
    drv_button_gesture_t *btn = (drv_button_gesture_t *)arg;
    drv_button_state_t state = ST_IDLE;
    TickType_t wait = portMAX_DELAY;
    drv_button_edge_t edge;

    for (;;) {
        if (xQueueReceive(btn->queue, &edge, wait) == pdTRUE) {
            switch (state) {
            case ST_IDLE:
                if (edge.pressed) {
                    state = ST_PRESSED;
                    wait = pdMS_TO_TICKS(btn->long_ms);
                }
                break;
            case ST_PRESSED:
                if (!edge.pressed) {
                    state = ST_WAIT_DOUBLE;
                    wait = pdMS_TO_TICKS(btn->double_gap_ms);
                }
                break;
            case ST_WAIT_DOUBLE:
                if (edge.pressed) {
                    drv_button_emit(btn, DRV_BUTTON_DOUBLE_PRESS);
                    state = ST_CONSUMED;
                    wait = portMAX_DELAY;
                }
                break;
            case ST_LONG_HELD:
                if (!edge.pressed) {
                    drv_button_emit(btn, DRV_BUTTON_RELEASE);
                    state = ST_IDLE;
                    wait = portMAX_DELAY;
                }
                break;
            case ST_CONSUMED:
                if (!edge.pressed) {
                    state = ST_IDLE;
                    wait = portMAX_DELAY;
                }
                break;
            }
            continue;
        }

        /* Timed out: whatever we were waiting for is not coming. */
        if (state == ST_PRESSED) {
            drv_button_emit(btn, DRV_BUTTON_LONG_PRESS);
            state = ST_LONG_HELD;
            wait = portMAX_DELAY;
        } else if (state == ST_WAIT_DOUBLE) {
            drv_button_emit(btn, DRV_BUTTON_SHORT_PRESS);
            state = ST_IDLE;
            wait = portMAX_DELAY;
        } else {
            wait = portMAX_DELAY;
        }
    }
}

esp_err_t drv_gpio_button_gesture_init(gpio_num_t pin, bool active_low, uint32_t debounce_ms,
                                       uint32_t long_ms, uint32_t double_gap_ms,
                                       drv_gpio_button_gesture_cb_t on_gesture, void *ctx)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err;

    if (on_gesture == NULL || long_ms == 0 || double_gap_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gesture.pin = pin;
    s_gesture.active_low = active_low;
    s_gesture.debounce_us = (int64_t)debounce_ms * 1000;
    s_gesture.last_us = 0;
    s_gesture.long_ms = long_ms;
    s_gesture.double_gap_ms = double_gap_ms;
    s_gesture.on_gesture = on_gesture;
    s_gesture.ctx = ctx;

    if (s_gesture.queue == NULL) {
        s_gesture.queue = xQueueCreate(DRV_BUTTON_QUEUE_LEN, sizeof(drv_button_edge_t));
        if (s_gesture.queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(drv_button_gesture_task, "btn_gesture", DRV_BUTTON_TASK_STACK, &s_gesture,
                        DRV_BUTTON_TASK_PRIO, NULL) != pdPASS) {
            vQueueDelete(s_gesture.queue);
            s_gesture.queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(pin, drv_button_gesture_isr, &s_gesture);
}
