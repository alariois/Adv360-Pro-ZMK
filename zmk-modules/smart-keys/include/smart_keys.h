/*
 * Internal API shared by the smart-keys behaviors.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <zmk/hid.h>

/* Modifiers currently masked by a mod-morph (tracked by wrapping
 * zmk_hid_masked_modifiers_set/clear at link time). */
zmk_mod_flags_t smart_keys_masked_mods(void);

/* True between Ctrl+Shift+U and the Enter/Space/Escape that ends the
 * unicode entry. */
bool smart_keys_unicode_active(void);
