#!/usr/bin/env python3
"""Applied inside the build container for split-radio builds (bin/build.sh):
Kinesis' LED code (zmk/app/src/rgb_underglow.c: status LEDs, backlight.c:
key backlight) only syncs the halves over Bluetooth. With split-radio the
left half is the central without Bluetooth: let these files take their
central path. The radio module provides zmk_split_bt_update_led(),
zmk_split_bt_update_bl() and zmk_ble_active_profile_*() on the left and
zmk_split_bt_peripheral_is_*() on the right, and carries the state."""
import sys

OVERRIDE = """
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RADIO) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* split-radio: the left half is the central, without Bluetooth */
#undef ZMK_BLE_IS_CENTRAL
#define ZMK_BLE_IS_CENTRAL 1
#endif
"""

for name in ("rgb_underglow.c", "backlight.c"):
    path = f"/app/zmk/app/src/{name}"
    s = open(path).read()
    if "split-radio" in s:
        continue
    anchor = "#include <zmk/ble.h>\n"
    if s.count(anchor) != 1:
        sys.exit(f"{name} changed: update the split-radio patch")
    s = s.replace(anchor, anchor + OVERRIDE)
    s = s.replace("#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)",
                  "#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) || IS_ENABLED(CONFIG_ZMK_SPLIT_RADIO)")
    open(path, "w").write(s)
    print(f"split-radio: {name} patched")
