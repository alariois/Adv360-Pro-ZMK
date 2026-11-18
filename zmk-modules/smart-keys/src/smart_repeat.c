/*
 * Smart repeat key: replays everything the last key press typed.
 *
 *  - A recording is every non-modifier keycode one key press produced, so
 *    macros (compose sequences like ScrollLock " o -> ö) replay whole.
 *  - Each keycode is stored with the modifiers it was really sent with
 *    (explicit mods minus mod-morph masking, plus implicit mods), and is
 *    replayed with exactly those: modifiers held while pressing repeat are
 *    masked. '=' typed with Shift held replays as '=', '>' as '>'.
 *  - Ctrl+Shift+U unicode entry is one recording, up to and including the
 *    Enter/Space that ends it.
 *  - Modifier-only and layer keys don't replace the recording, and neither
 *    does Space (so "> " + repeat gives "> >").
 *  - A single-key recording is pressed while repeat is held and released
 *    with it (OS auto-repeat works); if that key is still held, ZMK's HID
 *    layer releases it first, so repeat works mid-roll.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_smart_repeat

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

#include "smart_keys.h"

LOG_MODULE_REGISTER(smart_repeat, CONFIG_ZMK_LOG_LEVEL);

struct rec_item {
    uint16_t page;
    uint16_t id;
    zmk_mod_flags_t mods;
};

static struct rec_item rec[CONFIG_ZMK_SMART_REPEAT_MAX_KEYS];
static uint8_t rec_len;
static bool new_group = true; /* the next recorded keycode starts a new recording */
static bool unicode_session;
static bool replaying; /* our own replayed keycodes aren't recorded */

bool smart_keys_unicode_active(void) { return unicode_session; }

/* ---- recording ----------------------------------------------------------- */

static bool is_unicode_start(const struct rec_item *it) {
    return it->page == HID_USAGE_KEY && it->id == HID_USAGE_KEY_KEYBOARD_U &&
           (it->mods & (MOD_LCTL | MOD_RCTL)) && (it->mods & (MOD_LSFT | MOD_RSFT));
}

static bool ends_unicode(uint16_t id) {
    return id == HID_USAGE_KEY_KEYBOARD_RETURN_ENTER || id == HID_USAGE_KEY_KEYPAD_ENTER ||
           id == HID_USAGE_KEY_KEYBOARD_SPACEBAR || id == HID_USAGE_KEY_KEYBOARD_ESCAPE;
}

static void append(const struct rec_item *it) {
    if (rec_len < ARRAY_SIZE(rec)) {
        rec[rec_len++] = *it;
    }
}

static void record(const struct zmk_keycode_state_changed *ev) {
    struct rec_item it = {
        .page = ev->usage_page,
        .id = ev->keycode,
        .mods = (zmk_hid_get_explicit_mods() & ~smart_keys_masked_mods()) | ev->implicit_modifiers,
    };

    if (unicode_session) {
        append(&it);
        if (ends_unicode(it.id)) {
            unicode_session = false;
            new_group = true;
            if (it.id == HID_USAGE_KEY_KEYBOARD_ESCAPE) {
                rec_len = 0; /* cancelled entry: nothing to repeat */
            }
        }
        return;
    }

    if (is_unicode_start(&it)) {
        rec_len = 0;
        append(&it);
        unicode_session = true;
        new_group = false;
        return;
    }

    if (it.page == HID_USAGE_KEY && it.id == HID_USAGE_KEY_KEYBOARD_SPACEBAR) {
        return; /* repeat skips Space */
    }

    if (new_group) {
        rec_len = 0;
        new_group = false;
    }
    append(&it);
}

/* Module listeners run before ZMK's own, so this sees each event before the
 * HID listener applies it, with the modifier state it will be sent with. */
static int smart_repeat_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos != NULL) {
        if (pos->state && !unicode_session) {
            new_group = true;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev != NULL && ev->state && !replaying && ev->usage_page == HID_USAGE_KEY &&
        !is_mod(ev->usage_page, ev->keycode)) {
        record(ev);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(smart_repeat, smart_repeat_listener);
ZMK_SUBSCRIPTION(smart_repeat, zmk_position_state_changed);
ZMK_SUBSCRIPTION(smart_repeat, zmk_keycode_state_changed);

/* ---- replaying ----------------------------------------------------------- */

static struct rec_item play[CONFIG_ZMK_SMART_REPEAT_MAX_KEYS];
static uint8_t play_len, play_idx;
static bool play_release_next;
static bool single_held;

static void send(const struct rec_item *it, bool pressed) {
    if (pressed) {
        /* Exactly the recorded modifiers: hide whatever else is held. */
        zmk_hid_masked_modifiers_set(zmk_hid_get_explicit_mods() & ~it->mods);
    }
    replaying = true;
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = it->page,
        .keycode = it->id,
        .implicit_modifiers = it->mods,
        .explicit_modifiers = 0,
        .state = pressed,
        .timestamp = k_uptime_get(),
    });
    replaying = false;
    if (!pressed) {
        zmk_hid_masked_modifiers_clear();
    }
}

static void play_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(play_work, play_step);

static void play_step(struct k_work *work) {
    ARG_UNUSED(work);
    if (play_idx >= play_len) {
        return;
    }
    send(&play[play_idx], !play_release_next);
    if (play_release_next) {
        play_idx++;
    }
    play_release_next = !play_release_next;
    if (play_idx < play_len) {
        k_work_schedule(&play_work, K_MSEC(CONFIG_ZMK_SMART_REPEAT_STEP_MS));
    }
}

static int on_smart_repeat_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    if (rec_len == 0 || unicode_session || k_work_delayable_is_pending(&play_work)) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    memcpy(play, rec, rec_len * sizeof(rec[0]));
    play_len = rec_len;

    if (play_len == 1) {
        send(&play[0], true);
        single_held = true;
    } else {
        play_idx = 0;
        play_release_next = false;
        play_step(NULL);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_smart_repeat_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    if (single_held) {
        single_held = false;
        send(&play[0], false);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api smart_repeat_driver_api = {
    .binding_pressed = on_smart_repeat_pressed,
    .binding_released = on_smart_repeat_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int smart_repeat_init(const struct device *dev) { return 0; }

#define SMART_REPEAT_INST(n)                                                                       \
    BEHAVIOR_DT_INST_DEFINE(n, smart_repeat_init, NULL, NULL, NULL, POST_KERNEL,                   \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &smart_repeat_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SMART_REPEAT_INST)
