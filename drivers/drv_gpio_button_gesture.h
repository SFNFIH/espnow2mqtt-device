#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Gesture recognized on a momentary push button. */
typedef enum {
    DRV_BUTTON_SHORT_PRESS = 0, /**< Pressed and released quickly, once */
    DRV_BUTTON_DOUBLE_PRESS,    /**< Two short presses within the double-press gap */
    DRV_BUTTON_LONG_PRESS,      /**< Still held when the long-press threshold passed */
    DRV_BUTTON_RELEASE,         /**< Let go after a long press */
} drv_gpio_button_gesture_t;

/**
 * @brief Invoked in **task** context, once per recognized gesture.
 *
 * Unlike ::drv_gpio_button_cb_t this never runs in an interrupt, so it can call
 * blocking APIs directly.
 */
typedef void (*drv_gpio_button_gesture_cb_t)(drv_gpio_button_gesture_t gesture, void *ctx);

/**
 * @brief Watch a push button and classify presses into gestures.
 *
 * Where ::drv_gpio_button_init answers "was the button pressed", this answers
 * "how was it pressed", which is what a scene switch needs. Both edges are
 * watched and a small task runs the state machine, so the callback is free of
 * interrupt restrictions.
 *
 * A short press is only reported after @p double_gap_ms has passed without a
 * second press — that delay is inherent, because a first press is
 * indistinguishable from the start of a double press until the gap expires.
 * Keep it short (250–400 ms) or single presses will feel sluggish.
 *
 * @param pin            Button input, pulled to the idle level internally.
 * @param active_low     True when pressing pulls the pin to ground.
 * @param debounce_ms    Edges closer together than this are ignored. 20–50 is typical.
 * @param long_ms        Hold time that counts as a long press. 600–1000 is typical.
 * @param double_gap_ms  Maximum spacing between the two presses of a double press.
 * @param on_gesture     Runs in task context, once per gesture.
 * @param ctx            Passed back to @p on_gesture.
 */
esp_err_t drv_gpio_button_gesture_init(gpio_num_t pin, bool active_low, uint32_t debounce_ms,
                                       uint32_t long_ms, uint32_t double_gap_ms,
                                       drv_gpio_button_gesture_cb_t on_gesture, void *ctx);

#ifdef __cplusplus
}
#endif
