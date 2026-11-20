/*
 * Shared between both halves: the timestamped key event characteristic.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

/* Own UUIDs (not ZMK's split service): 5e1f71e0-... */
#define SPLIT_TIMING_UUID(n) BT_UUID_128_ENCODE(0x5e1f71e0 + (n), 0x3a52, 0x4c6e, 0x9a3d, 0x2d0b6c8e7f41)
#define SPLIT_TIMING_SERVICE_UUID SPLIT_TIMING_UUID(0)
#define SPLIT_TIMING_EVENT_UUID SPLIT_TIMING_UUID(1)

/* One key event, notified by the peripheral when its keys change. */
struct split_timing_event {
    uint8_t position;
    uint8_t pressed;
    uint32_t uptime_us; /* the peripheral's clock, wraps every ~71 minutes */
} __packed;

static inline uint32_t split_timing_now_us(void) {
    return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}
