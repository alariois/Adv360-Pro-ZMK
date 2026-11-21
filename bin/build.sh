#!/usr/bin/env bash

set -eu

PWD=$(pwd)
TIMESTAMP="${TIMESTAMP:-$(date -u +"%Y%m%d%H%M")}"
COMMIT="${COMMIT:-$(echo xxxxxx)}"
# split-timing first: its listener must see key events before all others
EXTRA_MODULES="${EXTRA_MODULES:-${PWD}/extra-modules/split-timing;${PWD}/extra-modules/split-radio;${PWD}/extra-modules/usb-bootloader-reset;${PWD}/extra-modules/key-monitor;${PWD}/extra-modules/smart-keys}"

# TEST=1 builds the test flavor: the left half (central) gets the key-monitor
# serial port that streams and injects key presses. Never use it day to day.
# The right half gets one too (injected presses then go over the split link).
LEFT_SNIPPETS="-S studio-rpc-usb-uart"
RIGHT_SNIPPETS=""
FLAVOR=""
if [ "${TEST:-}" = 1 ]; then
    LEFT_SNIPPETS="${LEFT_SNIPPETS} -S key-monitor"
    RIGHT_SNIPPETS="-S key-monitor"
    FLAVOR="-test"
fi

# Default (RADIO=1): no Bluetooth, the halves talk over the raw radio
# (zmk-modules/split-radio) with the pre-shared key SPLIT_RADIO_KEY; the
# left half on USB. RADIO=0: stock Bluetooth split, files named *-ble*.
if [ "${RADIO:-1}" = 1 ]; then
    [ ${#SPLIT_RADIO_KEY} = 32 ] || { echo "SPLIT_RADIO_KEY must be 32 hex digits (Makefile)" >&2; exit 1; }
    EXTRA_CMAKE="${EXTRA_CMAKE:-} -DCONFIG_BT=n -DCONFIG_ZMK_BLE=n -DCONFIG_ZMK_SPLIT_RADIO=y -DCONFIG_ZMK_BATTERY_REPORTING=y -DCONFIG_ZMK_SPLIT_RADIO_KEY=\"${SPLIT_RADIO_KEY}\""
    python3 "${PWD}/extra-modules/split-radio/patches/rgb_underglow.py"
else
    FLAVOR="-ble${FLAVOR}"
fi

# Experiments: a script (path inside the container) that patches the
# workspace before building, e.g. extra-modules/split-timing/experiments/*.sh
if [ -n "${ZEPHYR_PATCH:-}" ]; then
    sh "${ZEPHYR_PATCH}"
fi

# West Build (left)
west build -s zmk/app -p -d build/left -b adv360_left ${LEFT_SNIPPETS} -- -DZMK_CONFIG="${PWD}/config" -DZMK_EXTRA_MODULES="${EXTRA_MODULES}" ${EXTRA_CMAKE:-} -DCONFIG_ZMK_STUDIO=y
# Adv360 Left Kconfig file
grep -vE '(^#|^$)' build/left/zephyr/.config
# Rename zmk.uf2
cp build/left/zephyr/zmk.uf2 "./firmware/${TIMESTAMP}-${COMMIT}-left${FLAVOR}-clique.uf2"

# Build right side if selected
if [ "${BUILD_RIGHT}" = true ]; then
    # West Build (right)
    west build -s zmk/app -p -d build/right -b adv360_right ${RIGHT_SNIPPETS} -- -DZMK_CONFIG="${PWD}/config" -DZMK_EXTRA_MODULES="${EXTRA_MODULES}" ${EXTRA_CMAKE:-}
    # Adv360 Right Kconfig file
    grep -vE '(^#|^$)' build/right/zephyr/.config
    # Rename zmk.uf2
    cp build/right/zephyr/zmk.uf2 "./firmware/${TIMESTAMP}-${COMMIT}-right${FLAVOR}-clique.uf2"
fi

# Hand the firmware files back to the host user when built via the Makefile
if [ -n "${HOST_UID:-}" ]; then
    chown "${HOST_UID}:${HOST_GID:-${HOST_UID}}" ./firmware/*.uf2
fi
