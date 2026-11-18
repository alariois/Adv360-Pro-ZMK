#!/usr/bin/env bash

set -eu

PWD=$(pwd)
TIMESTAMP="${TIMESTAMP:-$(date -u +"%Y%m%d%H%M")}"
COMMIT="${COMMIT:-$(echo xxxxxx)}"
EXTRA_MODULES="${EXTRA_MODULES:-${PWD}/extra-modules/usb-bootloader-reset;${PWD}/extra-modules/key-monitor;${PWD}/extra-modules/smart-keys}"

# TEST=1 builds the test flavor: the left half (central) gets the key-monitor
# serial port that streams and injects key presses. Never use it day to day.
LEFT_SNIPPETS="-S studio-rpc-usb-uart"
FLAVOR=""
if [ "${TEST:-}" = 1 ]; then
    LEFT_SNIPPETS="${LEFT_SNIPPETS} -S key-monitor"
    FLAVOR="-test"
fi

# West Build (left)
west build -s zmk/app -p -d build/left -b adv360_left ${LEFT_SNIPPETS} -- -DZMK_CONFIG="${PWD}/config" -DZMK_EXTRA_MODULES="${EXTRA_MODULES}" -DCONFIG_ZMK_STUDIO=y
# Adv360 Left Kconfig file
grep -vE '(^#|^$)' build/left/zephyr/.config
# Rename zmk.uf2
cp build/left/zephyr/zmk.uf2 "./firmware/${TIMESTAMP}-${COMMIT}-left${FLAVOR}-clique.uf2"

# Build right side if selected
if [ "${BUILD_RIGHT}" = true ]; then
    # West Build (right)
    west build -s zmk/app -p -d build/right -b adv360_right -- -DZMK_CONFIG="${PWD}/config" -DZMK_EXTRA_MODULES="${EXTRA_MODULES}"
    # Adv360 Right Kconfig file
    grep -vE '(^#|^$)' build/right/zephyr/.config
    # Rename zmk.uf2
    cp build/right/zephyr/zmk.uf2 "./firmware/${TIMESTAMP}-${COMMIT}-right${FLAVOR}-clique.uf2"
fi

# Hand the firmware files back to the host user when built via the Makefile
if [ -n "${HOST_UID:-}" ]; then
    chown "${HOST_UID}:${HOST_GID:-${HOST_UID}}" ./firmware/*.uf2
fi
