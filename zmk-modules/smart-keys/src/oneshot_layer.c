/*
 * One-shot layer for the Hexpad, replacing a sticky layer:
 *
 *  - tap: the layer is on for the next (non-modifier) key, or until
 *    release-after-ms passes; Shift etc. don't use it up
 *  - tap again while it is still on: off
 *  - hold + other keys: momentary layer
 *  - hold, Ctrl+Shift+U unicode entry, release: sends the Enter that
 *    finishes the entry (unless Enter/Space already did)
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_oneshot_layer

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>

#include "smart_keys.h"

LOG_MODULE_REGISTER(oneshot_layer, CONFIG_ZMK_LOG_LEVEL);

/* Without the smart repeat behavior nothing tracks unicode entry. */
__weak bool smart_keys_unicode_active(void) { return false; }

struct oneshot_layer_config {
    uint32_t release_after_ms;
};

enum oneshot_mode { IDLE, HELD, ONESHOT };

/* One Hexpad key is active at a time, so the state is global. */
static enum oneshot_mode mode = IDLE;
static uint8_t active_layer;
static uint32_t own_position;
static bool used_while_held;
static bool swallow_release; /* the press that turned the one-shot off */

static void layer_off(void) {
    if (mode != IDLE) {
        mode = IDLE;
        zmk_keymap_layer_deactivate(active_layer);
    }
}

static void timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (mode == ONESHOT) {
        layer_off();
    }
}

static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_handler);

static void tap_enter(void) {
    for (int pressed = 1; pressed >= 0; pressed--) {
        raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
            .usage_page = HID_USAGE_KEY,
            .keycode = HID_USAGE_KEY_KEYBOARD_RETURN_ENTER,
            .state = pressed,
            .timestamp = k_uptime_get(),
        });
    }
}

static int on_oneshot_layer_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    if (mode == ONESHOT && active_layer == binding->param1) {
        k_work_cancel_delayable(&timeout_work);
        layer_off();
        swallow_release = true;
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (mode != IDLE) {
        k_work_cancel_delayable(&timeout_work);
        layer_off();
    }
    active_layer = binding->param1;
    own_position = event.position;
    used_while_held = false;
    mode = HELD;
    zmk_keymap_layer_activate(active_layer);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_oneshot_layer_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct oneshot_layer_config *cfg = dev->config;

    if (swallow_release) {
        swallow_release = false;
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (mode != HELD) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (used_while_held) {
        layer_off();
        if (smart_keys_unicode_active()) {
            tap_enter();
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }
    mode = ONESHOT;
    k_work_schedule(&timeout_work, K_MSEC(cfg->release_after_ms));
    return ZMK_BEHAVIOR_OPAQUE;
}

static int oneshot_layer_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos != NULL) {
        if (pos->state && mode == HELD && pos->position != own_position) {
            used_while_held = true;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* One-shot: the first non-modifier key uses it up. Its binding was
     * already resolved on this layer when its position was pressed. */
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev != NULL && ev->state && mode == ONESHOT && !is_mod(ev->usage_page, ev->keycode)) {
        k_work_cancel_delayable(&timeout_work);
        layer_off();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(oneshot_layer, oneshot_layer_listener);
ZMK_SUBSCRIPTION(oneshot_layer, zmk_position_state_changed);
ZMK_SUBSCRIPTION(oneshot_layer, zmk_keycode_state_changed);

static const struct behavior_driver_api oneshot_layer_driver_api = {
    .binding_pressed = on_oneshot_layer_pressed,
    .binding_released = on_oneshot_layer_released,
};

static int oneshot_layer_init(const struct device *dev) { return 0; }

#define ONESHOT_LAYER_INST(n)                                                                      \
    static const struct oneshot_layer_config oneshot_layer_config_##n = {                          \
        .release_after_ms = DT_INST_PROP(n, release_after_ms),                                     \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, oneshot_layer_init, NULL, NULL, &oneshot_layer_config_##n,          \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &oneshot_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ONESHOT_LAYER_INST)
