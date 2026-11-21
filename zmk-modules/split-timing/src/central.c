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

#include <zephyr/kernel.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#endif
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "split_timing.h"

LOG_MODULE_REGISTER(split_timing, CONFIG_ZMK_LOG_LEVEL);

#define DELAY_US ((int32_t)CONFIG_ZMK_SPLIT_TIMING_DELAY_MS * 1000)
#define PENDING_MAX_AGE_US (500 * 1000)

/* breadcrumbs for hang hunting (zmk-modules/usb-bootloader-reset) */
__weak void zmk_crumb(uint8_t id) {}

#ifdef CONFIG_ZMK_SPLIT_TIMING_WATERMARK_MARGIN_US
#define WATERMARK_MARGIN_US CONFIG_ZMK_SPLIT_TIMING_WATERMARK_MARGIN_US
#else
#define WATERMARK_MARGIN_US 0 /* watermark off: have_watermark stays false */
#endif

static inline int32_t tdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
/* ---- clock offset ---------------------------------------------------------- */

static uint32_t offset_samples[64];
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

#endif

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

/* ---- statistics: press -> processing delay ---------------------------------- */

struct delay_stats {
    uint32_t n, sum_us, max_us;
    uint32_t hist[32]; /* 1 ms buckets, last = 31+ ms */
};
static struct delay_stats stats[2]; /* 0 = this half, 1 = the other half */

static void note_delay(bool remote, int32_t d) {
    struct delay_stats *st = &stats[remote];
    uint32_t us = d > 0 ? d : 0;
    st->n++;
    st->sum_us += us;
    st->max_us = MAX(st->max_us, us);
    st->hist[MIN(us / 1000, ARRAY_SIZE(st->hist) - 1)]++;
}

static uint32_t percentile_ms(const struct delay_stats *st, uint32_t pct) {
    uint32_t want = (st->n * pct + 99) / 100, seen = 0;
    for (int i = 0; i < ARRAY_SIZE(st->hist); i++) {
        seen += st->hist[i];
        if (seen >= want) {
            return i + 1; /* upper bucket edge */
        }
    }
    return ARRAY_SIZE(st->hist);
}

/* For the key monitor's S command: "local n mean max p50 p90 p99 remote ..."
 * (us, percentiles as 1 ms bucket upper edges). */
int split_timing_stats(char *buf, size_t len, bool reset) {
    int off = 0;
    for (int r = 0; r < 2; r++) {
        const struct delay_stats *st = &stats[r];
        off += snprintf(buf + off, len - off, "%s%s n=%u mean=%u max=%u p50<%u p90<%u p99<%u",
                        r ? " " : "", r ? "remote" : "local", st->n, st->n ? st->sum_us / st->n : 0,
                        st->max_us, percentile_ms(st, 50) * 1000, percentile_ms(st, 90) * 1000,
                        percentile_ms(st, 99) * 1000);
    }
    if (reset) {
        memset(stats, 0, sizeof(stats));
    }
    return off;
}

/* ---- releasing -------------------------------------------------------------- */

/* Peripheral events with press times before this have all arrived (its
 * messages come in order), see note_watermark(). */
static uint32_t watermark;
static bool have_watermark;

static bool due(const struct entry *e, uint32_t now) {
    if (tdiff(now, e->t) >= DELAY_US) {
        return true;
    }
    if (!have_watermark || !e->timed) {
        return false;
    }
    /* an untimed peripheral event or a timestamp still waiting for its
     * ZMK event may be older than the watermark: wait for those */
    for (int i = 0; i < queue_len; i++) {
        if (!queue[i].timed) {
            return false;
        }
    }
    uint32_t wm = watermark;
    for (int i = 0; i < pending_len; i++) {
        if (tdiff(pending[i].t, wm) < 0) {
            wm = pending[i].t - 1;
        }
    }
    /* A peripheral event's own timestamp proves everything before it from
     * there has arrived. This half's keys need a margin: the peripheral's
     * press time estimates may be a little late. */
    bool remote = e->ev.data.source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
    return tdiff(wm, e->t) >= (remote ? 0 : WATERMARK_MARGIN_US);
}

static void release_first(uint32_t now) {
    struct entry e = queue[0];
    memmove(&queue[0], &queue[1], (queue_len - 1) * sizeof(queue[0]));
    queue_len--;
    zmk_crumb(12);
    note_delay(e.ev.data.source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, tdiff(now, e.t));
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

static void release_ready(void) {
    zmk_crumb(3);
    uint32_t now = split_timing_now_us();
    while (queue_len > 0 && due(&queue[0], now)) {
        release_first(now);
        now = split_timing_now_us();
    }
    schedule();
    zmk_crumb(4);
}

static void release_due(struct k_work *work) {
    ARG_UNUSED(work);
    release_ready();
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
    zmk_crumb(pos->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL ? 1 : 2);
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
    release_ready();
    return ZMK_EV_EVENT_CAPTURED;
}

ZMK_LISTENER(split_timing, split_timing_listener);
ZMK_SUBSCRIPTION(split_timing, zmk_position_state_changed);

/* ---- press times of peripheral events --------------------------------------- */

/* Gives the queued untimed event (pos, pressed) its press time, or keeps
 * the time for when ZMK's event arrives. */
static void note_remote_time(uint32_t position, bool pressed, uint32_t t, uint32_t arrival) {
    for (int i = 0; i < queue_len; i++) {
        struct entry *q = &queue[i];
        if (!q->timed && q->ev.data.position == position && q->ev.data.state == pressed) {
            q->t = t;
            q->timed = true;
            return;
        }
    }
    if (pending_len == ARRAY_SIZE(pending)) {
        memmove(&pending[0], &pending[1], (pending_len - 1) * sizeof(pending[0]));
        pending_len--;
    }
    pending[pending_len++] =
        (struct pending){.position = position, .pressed = pressed, .t = t, .arrival = arrival};
}

static void forget_old_pending(void) {
    uint32_t now = split_timing_now_us();
    while (pending_len > 0 && tdiff(now, pending[0].arrival) > PENDING_MAX_AGE_US) {
        memmove(&pending[0], &pending[1], (pending_len - 1) * sizeof(pending[0]));
        pending_len--;
    }
}

void split_timing_remote_time(uint32_t position, bool pressed, uint32_t press_us) {
    subscribed = true; /* this transport times every event */
    note_remote_time(position, pressed, press_us, split_timing_now_us());
    forget_old_pending();
    sort_queue();
    release_ready();
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
/* ---- timestamps from the peripheral over Bluetooth -------------------------- */

struct received {
    struct split_timing_event ev;
    uint32_t arrival;
};

K_MSGQ_DEFINE(received_msgq, sizeof(struct received), 32, 4);
static atomic_t reset_offset;

static void process_timestamps(struct k_work *work) {
    struct received r;
    ARG_UNUSED(work);
    zmk_crumb(5);
    if (atomic_clear(&reset_offset)) {
        offset_len = offset_idx = 0;
        pending_len = 0;
    }
    while (k_msgq_get(&received_msgq, &r, K_NO_WAIT) == 0) {
        note_offset(r.arrival - r.ev.uptime_us);
        uint32_t t = r.ev.uptime_us + offset - LINK_MIN_US;
        if (IS_ENABLED(CONFIG_ZMK_SPLIT_TIMING_WATERMARK)) {
            watermark = t;
            have_watermark = true;
        }
        if (r.ev.position != SPLIT_TIMING_HEARTBEAT) {
            note_remote_time(r.ev.position, r.ev.pressed, t, r.arrival);
        }
    }
    forget_old_pending(); /* timestamps whose ZMK event never came */
    sort_queue();
    release_ready();
}

static K_WORK_DEFINE(timestamps_work, process_timestamps);

/* Bluetooth RX thread: note the arrival time, process on the system work
 * queue like the key events themselves. */
static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length) {
    zmk_crumb(7);
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
    zmk_crumb(9);
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
    zmk_crumb(8);
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
    zmk_crumb(10);
    split_conn = bt_conn_ref(conn);
    atomic_set(&reset_offset, 1);
    /* after ZMK's own discovery of its split service */
    k_work_reschedule(&discover_work, K_MSEC(1500));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (conn != split_conn) {
        return;
    }
    zmk_crumb(11);
    subscribed = false;
    have_watermark = false;
    subscribe_params.value_handle = 0;
    bt_conn_unref(split_conn);
    split_conn = NULL;
    atomic_set(&reset_offset, 1);
}

BT_CONN_CB_DEFINE(split_timing_conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};
#endif /* CONFIG_ZMK_SPLIT_BLE */
