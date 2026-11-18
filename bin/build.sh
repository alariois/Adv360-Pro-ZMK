#!/usr/bin/env bash

set -eu

PWD=$(pwd)
TIMESTAMP="${TIMESTAMP:-$(date -u +"%Y%m%d%H%M")}"
COMMIT="${COMMIT:-$(echo xxxxxx)}"
EXTRA_MODULES="${EXTRA_MODULES:-${PWD}/extra-modules/usb-bootloader-reset}"

# West Build (left)
west build -s zmk/app -p -d build/left -b adv360_left -S studio-rpc-usb-uart -- -DZMK_CONFIG="${PWD}/config" -DZMK_EXTRA_MODULES="${EXTRA_MODULES}" -DCONFIG_ZMK_STUDIO=y
# Adv360 Left Kconfig file
grep -vE '(^#|^$)' build/left/zephyr/.config
# Rename zmk.uf2
cp build/left/zephyr/zmk.uf2 "./firmware/${TIMESTAMP}-${COMMIT}-left-clique.uf2"

# Build right side if selected
if [ "${BUILD_RIGHT}" = true ]; then
    # West Build (right)
    west build -s zmk/app -p -d build/right -b adv360_right -- -DZMK_CONFIG="${PWD}/config" -DZMK_EXTRA_MODULES="${EXTRA_MODULES}"
    # Adv360 Right Kconfig file
    grep -vE '(^#|^$)' build/right/zephyr/.config
    # Rename zmk.uf2
    cp build/right/zephyr/zmk.uf2 "./firmware/${TIMESTAMP}-${COMMIT}-right-clique.uf2"
fi

# Hand the firmware files back to the host user when built via the Makefile
if [ -n "${HOST_UID:-}" ]; then
    chown "${HOST_UID}:${HOST_GID:-${HOST_UID}}" ./firmware/*.uf2
fi
