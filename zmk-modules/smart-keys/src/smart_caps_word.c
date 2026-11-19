/*
 * Caps word that knows this keymap's compose macros and shows its state on
 * the caps lock LED. Otherwise like ZMK's &caps_word:
 *
 *  - the binding toggles it; letters get Shift (`mods`), letters, digits,
 *    modifiers and `continue-list` keys keep it going, anything else ends it.
 *  - Compose sequences (ScrollLock, accent, letter: š ž õ ä ö ü) keep it
 *    going and the letter gets Shift, so ö types Ö. When the accent is
 *    itself a letter (ScrollLock o o = °) nothing is shifted.
 *  - While active, the caps lock bit is added to the HID indicators the
 *    LEDs show (indicators.c wraps zmk_hid_indicators_get_current_profile).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_smart_caps_word

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
#include <zmk/hid.h>
#include <zmk/keys.h>

#include "smart_keys.h"

LOG_MODULE_REGISTER(smart_caps_word, CONFIG_ZMK_LOG_LEVEL);

struct continue_item {
    uint16_t page;
    uint32_t id;
    uint8_t implicit_modifiers;
};

struct caps_word_config {
    zmk_mod_flags_t mods;
    uint8_t continuations_count;
    struct continue_item continuations[];
};

static const struct caps_word_config *active_config; /* NULL = off */

enum compose_step { COMPOSE_NONE, COMPOSE_ACCENT, COMPOSE_LETTER };
static enum compose_step compose;
static bool compose_accent_is_letter;

bool smart_keys_caps_word_active(void) { return active_config != NULL; }

static void set_active(const struct caps_word_config *config) {
    active_config = config;
    compose = COMPOSE_NONE;
    LOG_DBG("caps word %s", config ? "on" : "off");
}

static bool is_alpha(uint32_t id) {
    return id >= HID_USAGE_KEY_KEYBOARD_A && id <= HID_USAGE_KEY_KEYBOARD_Z;
}

static bool is_numeric(uint32_t id) {
    return id >= HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION &&
           id <= HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS;
}

static bool in_continue_list(const struct caps_word_config *config,
                             const struct zmk_keycode_state_changed *ev) {
    for (int i = 0; i < config->continuations_count; i++) {
        const struct continue_item *c = &config->continuations[i];
        if (c->page == ev->usage_page && c->id == ev->keycode &&
            (c->implicit_modifiers & (ev->implicit_modifiers | zmk_hid_get_explicit_mods())) ==
                c->implicit_modifiers) {
            return true;
        }
    }
    return false;
}

static void shift(struct zmk_keycode_state_changed *ev) {
    ev->implicit_modifiers |= active_config->mods;
}

static int caps_word_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state || active_config == NULL || ev->usage_page != HID_USAGE_KEY ||
        is_mod(ev->usage_page, ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (compose) {
    case COMPOSE_ACCENT:
        compose_accent_is_letter = is_alpha(ev->keycode);
        compose = COMPOSE_LETTER;
        return ZMK_EV_EVENT_BUBBLE;
    case COMPOSE_LETTER:
        compose = COMPOSE_NONE;
        if (is_alpha(ev->keycode) && !compose_accent_is_letter) {
            shift(ev);
        }
        return ZMK_EV_EVENT_BUBBLE;
    case COMPOSE_NONE:
        break;
    }

    if (ev->keycode == HID_USAGE_KEY_KEYBOARD_SCROLL_LOCK) {
        compose = COMPOSE_ACCENT; /* this keymap's compose key */
    } else if (is_alpha(ev->keycode)) {
        shift(ev);
    } else if (!is_numeric(ev->keycode) && !in_continue_list(active_config, ev)) {
        set_active(NULL);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(smart_caps_word, caps_word_listener);
ZMK_SUBSCRIPTION(smart_caps_word, zmk_keycode_state_changed);

static int on_caps_word_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    set_active(active_config ? NULL : dev->config);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_caps_word_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api caps_word_driver_api = {
    .binding_pressed = on_caps_word_pressed,
    .binding_released = on_caps_word_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int caps_word_init(const struct device *dev) { return 0; }

#define CONTINUE_ITEM(i, n)                                                                        \
    {                                                                                              \
        .page = ZMK_HID_USAGE_PAGE(DT_INST_PROP_BY_IDX(n, continue_list, i)),                      \
        .id = ZMK_HID_USAGE_ID(DT_INST_PROP_BY_IDX(n, continue_list, i)),                          \
        .implicit_modifiers = SELECT_MODS(DT_INST_PROP_BY_IDX(n, continue_list, i)),               \
    }

#define CAPS_WORD_INST(n)                                                                          \
    static const struct caps_word_config caps_word_config_##n = {                                  \
        .mods = DT_INST_PROP_OR(n, mods, MOD_LSFT),                                                \
        .continuations = {LISTIFY(DT_INST_PROP_LEN(n, continue_list), CONTINUE_ITEM, (, ), n)},    \
        .continuations_count = DT_INST_PROP_LEN(n, continue_list),                                 \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, caps_word_init, NULL, NULL, &caps_word_config_##n, POST_KERNEL,     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &caps_word_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CAPS_WORD_INST)
