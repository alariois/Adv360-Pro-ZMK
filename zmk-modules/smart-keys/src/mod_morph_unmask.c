/*
 * Clear mod-morph modifier masking when the next key is pressed, see
 * CONFIG_ZMK_MOD_MORPH_UNMASK_ON_NEXT_KEY.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>

/* Runs before the keymap handles the press (module listeners link first),
 * so the new key's HID report already carries the real modifiers; a
 * mod-morph on the new key sets its own mask afterwards. */
static int mod_morph_unmask_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state) {
        zmk_hid_masked_modifiers_clear();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mod_morph_unmask, mod_morph_unmask_listener);
ZMK_SUBSCRIPTION(mod_morph_unmask, zmk_position_state_changed);
