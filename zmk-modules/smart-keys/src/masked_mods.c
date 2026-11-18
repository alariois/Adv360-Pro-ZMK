/*
 * Track mod-morph modifier masking. ZMK keeps the mask private in hid.c, but
 * the repeat key needs it to record the modifiers a key was really sent
 * with ('=' typed with Shift held but masked is a plain '=').
 * CMakeLists.txt links with --wrap for these two functions.
 *
 * SPDX-License-Identifier: MIT
 */

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
