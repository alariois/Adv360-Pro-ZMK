/*
 * Split timing, peripheral half: notify the central of every local key
 * event with the time it happened (see Kconfig).
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "split_timing.h"

LOG_MODULE_REGISTER(split_timing, CONFIG_ZMK_LOG_LEVEL);

BT_GATT_SERVICE_DEFINE(split_timing_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(SPLIT_TIMING_SERVICE_UUID)),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(SPLIT_TIMING_EVENT_UUID),
                                              BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL,
                                              NULL),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

K_MSGQ_DEFINE(event_msgq, sizeof(struct split_timing_event), 32, 4);

/* bt_gatt_notify can block for a buffer: not on the system work queue
 * (like ZMK's own split service). */
K_THREAD_STACK_DEFINE(notify_stack, 768);
static struct k_work_q notify_q;

static void notify_events(struct k_work *work) {
    struct split_timing_event ev;
    ARG_UNUSED(work);
    while (k_msgq_get(&event_msgq, &ev, K_NO_WAIT) == 0) {
        int err = bt_gatt_notify(NULL, &split_timing_svc.attrs[1], &ev, sizeof(ev));
        if (err && err != -ENOTCONN) {
            LOG_DBG("notify failed: %d", err);
        }
    }
}

static K_WORK_DEFINE(notify_work, notify_events);

static uint32_t last_queued_us; /* last timestamp queued (key or heartbeat) */

static int split_timing_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos != NULL && pos->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        struct split_timing_event ev = {
            .position = pos->position,
            .pressed = pos->state,
            .uptime_us = split_timing_now_us(),
        };
        if (k_msgq_put(&event_msgq, &ev, K_NO_WAIT) == 0) {
            last_queued_us = ev.uptime_us;
            k_work_submit_to_queue(&notify_q, &notify_work);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(split_timing, split_timing_listener);
ZMK_SUBSCRIPTION(split_timing, zmk_position_state_changed);

#if CONFIG_ZMK_SPLIT_TIMING_HEARTBEAT_US > 0
/* "Nothing pressed here before now": lets the central release its own keys
 * without waiting the full delay. */
static void heartbeat(struct k_timer *t) {
    /* Random period (+-1/3): a fixed one locks to the connection interval
     * (same 32 kHz clock), every heartbeat then sees the same latency and
     * the central's clock offset (smallest latency seen) is off by ms. */
    static uint32_t x = 2463534242u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    uint32_t third = CONFIG_ZMK_SPLIT_TIMING_HEARTBEAT_US / 3;
    k_timer_start(t, K_USEC(CONFIG_ZMK_SPLIT_TIMING_HEARTBEAT_US - third + x % (2 * third)),
                  K_NO_WAIT);

    struct split_timing_event ev = {
        .position = SPLIT_TIMING_HEARTBEAT,
        .uptime_us = split_timing_now_us(),
    };
    /* any timestamp is a watermark: skip when one went out recently or
     * the link is still busy sending (never crowd out key events) */
    if (ev.uptime_us - last_queued_us < CONFIG_ZMK_SPLIT_TIMING_HEARTBEAT_US / 2 ||
        k_msgq_num_used_get(&event_msgq) > 0) {
        return;
    }
    if (k_msgq_put(&event_msgq, &ev, K_NO_WAIT) == 0) {
        last_queued_us = ev.uptime_us;
        k_work_submit_to_queue(&notify_q, &notify_work);
    }
}
K_TIMER_DEFINE(heartbeat_timer, heartbeat, NULL);
#endif

static int split_timing_init(void) {
    k_work_queue_start(&notify_q, notify_stack, K_THREAD_STACK_SIZEOF(notify_stack),
                       K_PRIO_PREEMPT(5), NULL);
#if CONFIG_ZMK_SPLIT_TIMING_HEARTBEAT_US > 0
    k_timer_start(&heartbeat_timer, K_USEC(CONFIG_ZMK_SPLIT_TIMING_HEARTBEAT_US), K_NO_WAIT);
#endif
    return 0;
}

SYS_INIT(split_timing_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
