/*
 * Sticky Shift (see dts/bindings/behaviors/zmk,behavior-sticky-shift.yaml):
 *
 *  - pressed with Ctrl, Alt or Super held: the wrapped binding is a plain
 *    hold
 *  - held and any other key pressed meanwhile (also keys that type nothing,
 *    like the caps word key): a plain hold, released with the key
 *  - tapped alone: the binding stays held until the next key that types
 *    something (non-modifier keycode) has been handled completely - a whole
 *    macro like a compose letter gets the Shift, layer-only keys like the
 *    Hexpad don't use it up - or until release-after-ms; tapped again: off
 *    (on the Adv360 keymap the second tap lands on the Shift layer, where
 *    the Shift keys are caps word instead)
 *
 * ZMK's sticky key counts only keycodes as "another key" and keeps one
 * sticky key per key position, so it can't hold a Shift + layer macro like
 * this.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_sticky_shift

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>

LOG_MODULE_REGISTER(sticky_shift, CONFIG_ZMK_LOG_LEVEL);

#define NOT_STICKY_MODS (MOD_LCTL | MOD_RCTL | MOD_LALT | MOD_RALT | MOD_LGUI | MOD_RGUI)

struct sticky_shift_config {
    struct zmk_behavior_binding binding;
    uint32_t release_after_ms;
};

enum sticky_mode { IDLE, PLAIN, HELD, STICKY };

struct sticky_shift_data {
    const struct sticky_shift_config *cfg;
    enum sticky_mode mode;
    struct zmk_behavior_binding_event event; /* of the press, for the release */
    bool used_while_held;
    bool swallow_release; /* the press that turned it off */
    struct k_work_delayable timeout_work;
    struct k_work release_work;
};

#define MAX_INSTANCES 4
static struct sticky_shift_data *instances[MAX_INSTANCES];

static void inner(struct sticky_shift_data *d, bool pressed) {
    struct zmk_behavior_binding_event ev = d->event;
    ev.timestamp = k_uptime_get();
    zmk_behavior_invoke_binding(&d->cfg->binding, ev, pressed);
}

static void release_inner(struct sticky_shift_data *d) {
    if (d->mode != IDLE) {
        d->mode = IDLE;
        k_work_cancel_delayable(&d->timeout_work);
        inner(d, false);
    }
}

static void timeout_handler(struct k_work *work) {
    struct k_work_delayable *dw = k_work_delayable_from_work(work);
    struct sticky_shift_data *d = CONTAINER_OF(dw, struct sticky_shift_data, timeout_work);
    if (d->mode == STICKY) {
        release_inner(d);
    }
}

/* Runs on the system work queue after the key that used the sticky Shift
 * up has been handled completely (its whole macro included). */
static void used_handler(struct k_work *work) {
    struct sticky_shift_data *d = CONTAINER_OF(work, struct sticky_shift_data, release_work);
    if (d->mode == STICKY) {
        release_inner(d);
    }
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct sticky_shift_data *d = dev->data;

    if (d->mode == STICKY) { /* tapped again: off */
        release_inner(d);
        d->swallow_release = true;
        return ZMK_BEHAVIOR_OPAQUE;
    }
    release_inner(d);
    d->event = event;
    d->used_while_held = false;
    d->mode = (zmk_hid_get_explicit_mods() & NOT_STICKY_MODS) ? PLAIN : HELD;
    inner(d, true);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct sticky_shift_data *d = dev->data;

    if (d->swallow_release) {
        d->swallow_release = false;
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (d->mode == PLAIN || (d->mode == HELD && d->used_while_held)) {
        release_inner(d);
    } else if (d->mode == HELD) {
        d->mode = STICKY;
        k_work_schedule(&d->timeout_work, K_MSEC(d->cfg->release_after_ms));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int sticky_shift_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos != NULL) {
        if (pos->state) {
            for (int i = 0; i < MAX_INSTANCES && instances[i]; i++) {
                struct sticky_shift_data *d = instances[i];
                if (d->mode == HELD && pos->position != d->event.position) {
                    d->used_while_held = true;
                }
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev != NULL && ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        for (int i = 0; i < MAX_INSTANCES && instances[i]; i++) {
            struct sticky_shift_data *d = instances[i];
            if (d->mode == STICKY) {
                k_work_cancel_delayable(&d->timeout_work);
                k_work_submit(&d->release_work); /* after this key is done */
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sticky_shift, sticky_shift_listener);
ZMK_SUBSCRIPTION(sticky_shift, zmk_position_state_changed);
ZMK_SUBSCRIPTION(sticky_shift, zmk_keycode_state_changed);

static const struct behavior_driver_api sticky_shift_driver_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

static int sticky_shift_init(const struct device *dev) {
    struct sticky_shift_data *d = dev->data;
    d->cfg = dev->config;
    k_work_init_delayable(&d->timeout_work, timeout_handler);
    k_work_init(&d->release_work, used_handler);
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (instances[i] == NULL) {
            instances[i] = d;
            break;
        }
    }
    return 0;
}

#define STICKY_SHIFT_INST(n)                                                                       \
    static const struct sticky_shift_config sticky_shift_config_##n = {                            \
        .binding = ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                                  \
        .release_after_ms = DT_INST_PROP(n, release_after_ms),                                     \
    };                                                                                             \
    static struct sticky_shift_data sticky_shift_data_##n;                                         \
    BEHAVIOR_DT_INST_DEFINE(n, sticky_shift_init, NULL, &sticky_shift_data_##n,                    \
                            &sticky_shift_config_##n, POST_KERNEL,                                 \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &sticky_shift_driver_api);

DT_INST_FOREACH_STATUS_OKAY(STICKY_SHIFT_INST)
