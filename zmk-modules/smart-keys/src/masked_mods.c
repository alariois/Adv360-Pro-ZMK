/*
 * Track mod-morph modifier masking. ZMK keeps the mask private in hid.c, but
 * the repeat key needs it to record the modifiers a key was really sent
 * with ('=' typed with Shift held but masked is a plain '=').
 * CMakeLists.txt links with --wrap for these two functions.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/toolchain.h>
#include <zmk/hid.h>

#include "smart_keys.h"

static zmk_mod_flags_t masked;

int __real_zmk_hid_masked_modifiers_set(zmk_mod_flags_t masked_modifiers);
int __real_zmk_hid_masked_modifiers_clear(void);

int __wrap_zmk_hid_masked_modifiers_set(zmk_mod_flags_t masked_modifiers) {
    masked = masked_modifiers;
    return __real_zmk_hid_masked_modifiers_set(masked_modifiers);
}

int __wrap_zmk_hid_masked_modifiers_clear(void) {
    masked = 0;
    return __real_zmk_hid_masked_modifiers_clear();
}

zmk_mod_flags_t smart_keys_masked_mods(void) { return masked; }

/* The HID listener applies a key's implicit modifiers after every other
 * listener (e.g. caps word adding Shift) had its say: report them to the
 * repeat recorder. */
int __real_zmk_hid_implicit_modifiers_press(zmk_mod_flags_t implicit_modifiers);

__weak void smart_keys_implicit_mods_applied(zmk_mod_flags_t implicit_modifiers) {}

int __wrap_zmk_hid_implicit_modifiers_press(zmk_mod_flags_t implicit_modifiers) {
    smart_keys_implicit_mods_applied(implicit_modifiers);
    return __real_zmk_hid_implicit_modifiers_press(implicit_modifiers);
}
