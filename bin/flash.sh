#!/usr/bin/env bash
#
# Build (optional) and flash both halves of the Advantage 360 Pro in one go.
#
# Each half is rebooted into its UF2 bootloader by a "1200 baud touch" on its
# USB serial port (see zmk-modules/usb-bootloader-reset). If a half has no such
# port (e.g. it still runs older firmware) you're asked to press
# Mod + bootloader key on that half instead.
#
# Halves are told apart by USB serial number: the firmware and the bootloader
# both report the nRF52 chip id, so the bootloader drive of each half can be
# matched to it. Learned serials are cached for when a half is already sitting
# in the bootloader.
#
# Usage: bin/flash.sh [options]
#   --no-build        don't run `make`, flash the newest firmware/*.uf2
#   --test            build/flash the TEST flavor: the left half gets the
#                     key-monitor port that streams and injects key presses.
#                     Flash without --test to go back to daily firmware.
#   --left | --right  flash only one half
#   --reset           flash settings-reset.uf2 first (needs a manual
#                     bootloader key press afterwards, the reset firmware has
#                     no serial port)
#   --left-uf2 FILE   flash FILE on the left half instead of the newest build
#   --right-uf2 FILE  same for the right half (both imply --no-build)
#   --timeout SEC     how long to wait for each step (default 30)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VID="29ea"
declare -A PRODUCT=([left]="Adv360 Pro" [right]="Adv360 Pro rt")
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/adv360-flash/serials"

build=true
sides=(left right)
reset=false
flavor=""
timeout=30
declare -A FW=()

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) build=false ;;
        --test) flavor="-test" ;;
        --left) sides=(left) ;;
        --right) sides=(right) ;;
        --reset) reset=true ;;
        --left-uf2) FW[left]="$(realpath "$2")"; build=false; sides=(left); shift ;;
        --right-uf2) FW[right]="$(realpath "$2")"; build=false; sides=(right); shift ;;
        --timeout) timeout="$2"; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# sysfs dir of the half (in normal, non-bootloader mode) with product $1.
find_usb_device() {
    local d
    for d in /sys/bus/usb/devices/*; do
        [ "$(cat "$d/idVendor" 2>/dev/null)" = "$VID" ] || continue
        [ "$(cat "$d/product" 2>/dev/null)" = "$1" ] || continue
        readlink -f "$d"
        return 0
    done
    return 1
}

# /dev/ttyACM* that belongs to the USB device in sysfs dir $1.
find_serial_port() {
    local tty
    for tty in /sys/class/tty/ttyACM*; do
        [ -e "$tty" ] || continue
        [ "$(readlink -f "$tty/device/..")" = "$1" ] || continue
        echo "/dev/$(basename "$tty")"
        return 0
    done
    return 1
}

cache_get() { [ -f "$CACHE" ] && awk -v s="$1" '$1 == s { print $2 }' "$CACHE" || true; }
cache_put() {
    mkdir -p "$(dirname "$CACHE")"
    { [ -f "$CACHE" ] && grep -v "^$1 " "$CACHE" || true; echo "$1 $2"; } >"$CACHE.tmp"
    mv "$CACHE.tmp" "$CACHE"
}

# UF2 bootloader drives as "<device> <usb serial>" lines.
bootloader_drives() {
    local dev props
    for dev in /dev/sd[a-z]; do
        [ -b "$dev" ] || continue
        props="$(udevadm info -q property -n "$dev" 2>/dev/null)" || continue
        grep -qx 'ID_MODEL=nRF_UF2' <<<"$props" || continue
        grep -qx 'ID_FS_LABEL=ADV360PRO' <<<"$props" || continue
        echo "$dev $(sed -n 's/^ID_SERIAL_SHORT=//p' <<<"$props")"
    done
}

# Bootloader drive of the half with USB serial $1. With an unknown serial,
# fall back to the only drive whose serial isn't one of the other halves'.
find_bootloader_drive() {
    local serial="$1" others="$2" dev ser candidates=()
    while read -r dev ser; do
        [ -n "$dev" ] || continue
        if [ -n "$serial" ]; then
            [ "$ser" = "$serial" ] && { echo "$dev"; return 0; }
        elif ! grep -qxF "$ser" <<<"$others"; then
            candidates+=("$dev")
        fi
    done < <(bootloader_drives)
    [ ${#candidates[@]} -eq 1 ] && { echo "${candidates[0]}"; return 0; }
    return 1
}

# Print the mount point of $1, mounting it via udisks if the desktop didn't.
mount_drive() {
    local dev="$1" mnt i
    for i in $(seq 1 6); do
        mnt="$(findmnt -rno TARGET -S "$dev" | head -n1 || true)"
        [ -n "$mnt" ] && { echo "$mnt"; return 0; }
        sleep 0.5
    done
    udisksctl mount --no-user-interaction -b "$dev" >/dev/null
    findmnt -rno TARGET -S "$dev" | head -n1
}

declare -A SERIAL

# Learn each half's serial while it is running normally, and kick it into the
# bootloader. Prompts for a key press when there's no serial port.
enter_bootloader() {
    local side="$1" usbdev="" port i
    # A half that was just plugged in / rebooted may still be enumerating.
    for i in $(seq 1 10); do
        usbdev="$(find_usb_device "${PRODUCT[$side]}")" && break
        find_bootloader_drive "$(cache_get "$side")" "" >/dev/null && break
        sleep 0.5
    done
    if [ -n "$usbdev" ]; then
        SERIAL[$side]="$(cat "$usbdev/serial" 2>/dev/null || true)"
        [ -n "${SERIAL[$side]}" ] && cache_put "$side" "${SERIAL[$side]}"
        if port="$(find_serial_port "$usbdev")"; then
            log "$side: 1200 baud touch on $port"
            stty -F "$port" 1200 2>/dev/null || true
            return 0
        fi
    else
        SERIAL[$side]="$(cache_get "$side")"
    fi
    if [ -n "${SERIAL[$side]}" ] && find_bootloader_drive "${SERIAL[$side]}" "" >/dev/null; then
        log "$side: already in the bootloader"
        return 0
    fi
    warn "$side: no serial port, press Mod + bootloader key on the ${side^^} half now"
}

# Copy $2 onto the bootloader drive of $1 and wait for the half to reboot.
flash_side() {
    local side="$1" uf2="$2" others="" s dev mnt deadline
    for s in "${!SERIAL[@]}"; do [ "$s" = "$side" ] || others+="${SERIAL[$s]}"$'\n'; done

    deadline=$((SECONDS + timeout))
    until dev="$(find_bootloader_drive "${SERIAL[$side]}" "$others")"; do
        [ $SECONDS -lt $deadline ] || die "$side: bootloader drive didn't show up within ${timeout}s"
        sleep 0.5
    done
    if [ -z "${SERIAL[$side]}" ]; then
        SERIAL[$side]="$(udevadm info -q property -n "$dev" | sed -n 's/^ID_SERIAL_SHORT=//p')"
        cache_put "$side" "${SERIAL[$side]}"
    fi
    mnt="$(mount_drive "$dev")" || die "$side: couldn't mount $dev"
    log "$side: writing $(basename "$uf2") to $dev ($mnt)"

    # The bootloader reboots as soon as the whole file has arrived, so the
    # copy/sync can fail with an I/O error at the very end. That's expected.
    cp "$uf2" "$mnt/" 2>/dev/null || true
    sync 2>/dev/null || true

    deadline=$((SECONDS + timeout))
    while [ -b "$dev" ] && [ $SECONDS -lt $deadline ]; do sleep 0.5; done
    [ -b "$dev" ] && die "$side: didn't leave the bootloader, is $uf2 valid?"
    log "$side: rebooted"
}

# Wait for the half to come back running the new firmware.
wait_for_boot() {
    local side="$1" deadline=$((SECONDS + timeout)) usbdev port
    while [ $SECONDS -lt $deadline ]; do
        if usbdev="$(find_usb_device "${PRODUCT[$side]}")" && port="$(find_serial_port "$usbdev")"; then
            log "$side: back up, serial port $port"
            return 0
        fi
        sleep 0.5
    done
    warn "$side: no serial port after reboot, the next flash will need a key press"
}

if $build; then
    log "Building firmware"
    if [ -n "$flavor" ]; then make -C "$ROOT" test; else make -C "$ROOT"; fi
fi

[ ${#FW[@]} -eq 2 ] && sides=(left right)
for side in "${sides[@]}"; do
    [ -n "${FW[$side]:-}" ] && { [ -f "${FW[$side]}" ] || die "No such file: ${FW[$side]}"; SERIAL[$side]=""; continue; }
    FW[$side]="$(ls -1t "$ROOT"/firmware/*-"$side$flavor"-clique.uf2 2>/dev/null | head -n1 || true)"
    [ -n "${FW[$side]}" ] || die "No firmware/*-$side$flavor-clique.uf2 found, run without --no-build"
    SERIAL[$side]=""
done

if $reset; then
    for side in "${sides[@]}"; do enter_bootloader "$side"; done
    for side in "${sides[@]}"; do flash_side "$side" "$ROOT/settings-reset.uf2"; done
fi

for side in "${sides[@]}"; do enter_bootloader "$side"; done
for side in "${sides[@]}"; do flash_side "$side" "${FW[$side]}"; done
for side in "${sides[@]}"; do wait_for_boot "$side"; done

log "Done"
