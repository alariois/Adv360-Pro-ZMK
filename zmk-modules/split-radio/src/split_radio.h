/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct split_radio_event {
    uint8_t position;
    bool pressed;
    uint32_t press_us; /* the left half's clock */
};

struct split_radio_stats {
    uint32_t frames, retries, crc_errors, bad, hellos, resyncs, last_rtt_us;
};

/* left half: raise one received key event (system work queue) */
void split_radio_deliver(const struct split_radio_event *ev);

const struct split_radio_stats *split_radio_get_stats(void);
