/*
 * Split timing, central half: process the key events of both halves in the
 * order they were pressed (see Kconfig).
 *
 *  - Every position event (local or from the peripheral) is captured and
 *    queued by its press time on this half's clock, then released to the
 *    rest of ZMK DELAY after that press time.
 *  - Local events: press time = now.
 *  - Peripheral events come from ZMK's own split service (they stay the
 *    source of truth: nothing is lost if timing data goes missing). Their
 *    press time comes from this module's timestamped notifications, matched
 *    by position and state. An event whose timestamp hasn't arrived waits
 *    with its arrival time until it does.
 *  - Clock offset: the smallest (arrival - peripheral time) of the last
 *    samples = offset + the fastest link latency; drift is tracked by the
 *    sliding window. That places peripheral keys late by the fastest
 *    latency, so LINK_MIN_US (measured, ~2.5 ms) is subtracted.
 *
 * Position events raised again by later listeners (ZMK combos re-raise
 * captured keys) would be delayed a second time; this keymap has no combos.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "split_timing.h"

LOG_MODULE_REGISTER(split_timing, CONFIG_ZMK_LOG_LEVEL);

#define DELAY_US ((int32_t)CONFIG_ZMK_SPLIT_TIMING_DELAY_MS * 1000)
#define PENDING_MAX_AGE_US (500 * 1000)

static inline int32_t tdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

/* ---- clock offset ---------------------------------------------------------- */

static uint32_t offset_samples[32];
static uint8_t offset_len, offset_idx;
static uint32_t offset; /* peripheral time + offset = press time here */
#define LINK_MIN_US CONFIG_ZMK_SPLIT_TIMING_LINK_MIN_US

static void note_offset(uint32_t sample) {
    offset_samples[offset_idx] = sample;
    offset_idx = (offset_idx + 1) % ARRAY_SIZE(offset_samples);
    if (offset_len < ARRAY_SIZE(offset_samples)) {
        offset_len++;
    }
    uint32_t best = sample;
    for (int i = 0; i < offset_len; i++) {
        if (tdiff(offset_samples[i], best) < 0) {
            best = offset_samples[i];
        }
    }
    offset = best;
}

/* ---- the press-time ordered queue ------------------------------------------ */

struct entry {
    struct zmk_position_state_changed_event ev;
    uint32_t t;   /* press time, this half's clock (us) */
    uint32_t seq; /* arrival order */
    bool timed;   /* false: a peripheral event still waiting for its timestamp */
};

static struct entry queue[32];
static uint8_t queue_len;
static uint32_t next_seq;
static bool subscribed; /* the peripheral sends timestamps */

/* Timestamps that arrived before ZMK's event for them. */
struct pending {
    uint8_t position;
    bool pressed;
    uint32_t t;
    uint32_t arrival;
};

static struct pending pending[16];
static uint8_t pending_len;

static void release_due(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(release_work, release_due);

static void sort_queue(void) {
    /* A key's own events keep their arrival order (press before release). */
    for (int i = 0; i < queue_len; i++) {
        for (int j = 0; j < queue_len; j++) {
            if (queue[i].ev.data.position == queue[j].ev.data.position &&
                queue[i].seq < queue[j].seq && tdiff(queue[j].t, queue[i].t) <= 0) {
                queue[j].t = queue[i].t + 1;
            }
        }
    }
    /* insertion sort by press time, ties in arrival order */
    for (int i = 1; i < queue_len; i++) {
        struct entry e = queue[i];
        int j = i;
        while (j > 0 && (tdiff(queue[j - 1].t, e.t) > 0 ||
                         (queue[j - 1].t == e.t && queue[j - 1].seq > e.seq))) {
            queue[j] = queue[j - 1];
            j--;
        }
        queue[j] = e;
    }
}

static void release_first(uint32_t now) {
    struct entry e = queue[0];
    memmove(&queue[0], &queue[1], (queue_len - 1) * sizeof(queue[0]));
    queue_len--;
    /* ZMK's timestamps (hold-tap, combos) become the press time too */
    e.ev.data.timestamp = k_uptime_get() - MAX(tdiff(now, e.t), 0) / 1000;
    ZMK_EVENT_RELEASE(e.ev);
}

static void schedule(void) {
    if (queue_len == 0) {
        return;
    }
    int32_t wait = DELAY_US - tdiff(split_timing_now_us(), queue[0].t);
    k_work_reschedule(&release_work, wait > 0 ? K_USEC(wait) : K_NO_WAIT);
}

static void release_due(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t now = split_timing_now_us();
    while (queue_len > 0 && tdiff(now, queue[0].t) >= DELAY_US) {
        release_first(now);
        now = split_timing_now_us();
    }
    schedule();
}

static int take_pending(uint32_t position, bool pressed, uint32_t *t) {
    for (int i = 0; i < pending_len; i++) {
        if (pending[i].position == position && pending[i].pressed == pressed) {
            *t = pending[i].t;
            memmove(&pending[i], &pending[i + 1], (pending_len - i - 1) * sizeof(pending[0]));
            pending_len--;
            return 0;
        }
    }
    return -ENOENT;
}

static int split_timing_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    uint32_t now = split_timing_now_us();
    struct entry e = {
        .ev = copy_raised_zmk_position_state_changed(pos),
        .t = now,
        .seq = next_seq++,
        .timed = true,
    };
    if (pos->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL &&
        take_pending(pos->position, pos->state, &e.t) != 0) {
        /* wait for the timestamp only if the peripheral sends them */
        e.timed = !subscribed;
    }
    if (queue_len == ARRAY_SIZE(queue)) {
        release_first(now); /* full: let the oldest go early */
    }
    queue[queue_len++] = e;
    sort_queue();
    schedule();
    return ZMK_EV_EVENT_CAPTURED;
}

ZMK_LISTENER(split_timing, split_timing_listener);
ZMK_SUBSCRIPTION(split_timing, zmk_position_state_changed);

/* ---- timestamps from the peripheral ----------------------------------------- */

struct received {
    struct split_timing_event ev;
    uint32_t arrival;
};

K_MSGQ_DEFINE(received_msgq, sizeof(struct received), 32, 4);
static atomic_t reset_offset;

static void process_timestamps(struct k_work *work) {
    struct received r;
    ARG_UNUSED(work);
    if (atomic_clear(&reset_offset)) {
        offset_len = offset_idx = 0;
        pending_len = 0;
    }
    while (k_msgq_get(&received_msgq, &r, K_NO_WAIT) == 0) {
        note_offset(r.arrival - r.ev.uptime_us);
        uint32_t t = r.ev.uptime_us + offset - LINK_MIN_US;
        bool matched = false;
        for (int i = 0; i < queue_len; i++) {
            struct entry *q = &queue[i];
            if (!q->timed && q->ev.data.position == r.ev.position &&
                q->ev.data.state == (bool)r.ev.pressed) {
                q->t = t;
                q->timed = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            if (pending_len == ARRAY_SIZE(pending)) {
                memmove(&pending[0], &pending[1], (pending_len - 1) * sizeof(pending[0]));
                pending_len--;
            }
            pending[pending_len++] = (struct pending){
                .position = r.ev.position, .pressed = r.ev.pressed, .t = t, .arrival = r.arrival};
        }
    }
    /* forget timestamps whose ZMK event never came */
    uint32_t now = split_timing_now_us();
    while (pending_len > 0 && tdiff(now, pending[0].arrival) > PENDING_MAX_AGE_US) {
        memmove(&pending[0], &pending[1], (pending_len - 1) * sizeof(pending[0]));
        pending_len--;
    }
    sort_queue();
    schedule();
}

static K_WORK_DEFINE(timestamps_work, process_timestamps);

/* Bluetooth RX thread: note the arrival time, process on the system work
 * queue like the key events themselves. */
static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length) {
    if (data == NULL) {
        params->value_handle = 0;
        subscribed = false;
        return BT_GATT_ITER_STOP;
    }
    if (length == sizeof(struct split_timing_event)) {
        struct received r = {.arrival = split_timing_now_us()};
        memcpy(&r.ev, data, sizeof(r.ev));
        if (k_msgq_put(&received_msgq, &r, K_NO_WAIT) == 0) {
            k_work_submit(&timestamps_work);
        }
    }
    return BT_GATT_ITER_CONTINUE;
}

/* ---- subscribing on the split connection ------------------------------------ */

static struct bt_conn *split_conn;
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_discover_params ccc_discover_params;
static struct bt_gatt_subscribe_params subscribe_params;
static const struct bt_uuid_128 event_uuid = BT_UUID_INIT_128(SPLIT_TIMING_EVENT_UUID);

static void discover(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(discover_work, discover);

static uint8_t on_discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params) {
    if (attr == NULL) {
        LOG_WRN("peripheral has no split timing (old firmware?): arrival order");
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    subscribe_params.notify = on_notify;
    subscribe_params.value = BT_GATT_CCC_NOTIFY;
    subscribe_params.value_handle = chrc->value_handle;
    subscribe_params.ccc_handle = 0; /* auto-discovered */
    subscribe_params.end_handle = 0xffff;
    subscribe_params.disc_params = &ccc_discover_params;
    int err = bt_gatt_subscribe(conn, &subscribe_params);
    if (err && err != -EALREADY) {
        LOG_ERR("subscribe failed: %d", err);
        k_work_reschedule(&discover_work, K_MSEC(1000));
    } else {
        subscribed = true;
        LOG_INF("split timing on");
    }
    return BT_GATT_ITER_STOP;
}

static void discover(struct k_work *work) {
    ARG_UNUSED(work);
    if (split_conn == NULL) {
        return;
    }
    discover_params.uuid = &event_uuid.uuid;
    discover_params.func = on_discovered;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    int err = bt_gatt_discover(split_conn, &discover_params);
    if (err) {
        LOG_DBG("discover failed: %d, retrying", err);
        k_work_reschedule(&discover_work, K_MSEC(1000));
    }
}

/* This half is the Bluetooth central only towards the other half (hosts
 * connect to it as a peripheral). */
static void on_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;
    if (err || split_conn != NULL || bt_conn_get_info(conn, &info) != 0 ||
        info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    split_conn = bt_conn_ref(conn);
    atomic_set(&reset_offset, 1);
    /* after ZMK's own discovery of its split service */
    k_work_reschedule(&discover_work, K_MSEC(1500));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (conn != split_conn) {
        return;
    }
    subscribed = false;
    subscribe_params.value_handle = 0;
    bt_conn_unref(split_conn);
    split_conn = NULL;
    atomic_set(&reset_offset, 1);
}

BT_CONN_CB_DEFINE(split_timing_conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};
