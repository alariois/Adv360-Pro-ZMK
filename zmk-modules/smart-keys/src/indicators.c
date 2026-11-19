/*
 * Caps word lights the caps lock LED: the LED code (rgb_underglow.c) polls
 * zmk_hid_indicators_get_current_profile(), which CMakeLists.txt wraps at
 * link time to add the caps lock bit while caps word is on.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/hid_indicators.h>

#include "smart_keys.h"

#define CAPS_LOCK_BIT BIT(1) /* HID LED report bit (ZMK_LED_CAPSLOCK_BIT) */

zmk_hid_indicators_t __real_zmk_hid_indicators_get_current_profile(void);

zmk_hid_indicators_t __wrap_zmk_hid_indicators_get_current_profile(void) {
    zmk_hid_indicators_t indicators = __real_zmk_hid_indicators_get_current_profile();
    return smart_keys_caps_word_active() ? (indicators | CAPS_LOCK_BIT) : indicators;
}
