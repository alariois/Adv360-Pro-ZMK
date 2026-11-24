/*
 * Split radio: the two halves talk over the nRF52840 radio directly
 * (Nordic proprietary 2 Mbit mode, one channel, no Bluetooth), see Kconfig.
 *
 * Right half (peripheral, battery, "PTX"): every key event is queued with
 * its press time and sent at once in a DATA frame (up to MAX_EVENTS events,
 * each with its age in us when the frame was built). Without key events it
 * polls every POLL_MS with an empty DATA frame. The left half answers every
 * valid frame with an ACK carrying its LED state (layer, indicators, ...).
 * Without an ack the frame is rebuilt (fresh ages, new counter) and resent
 * after a backoff that grows while the left half doesn't answer. After
 * sending, the radio switches to receiving by itself (hardware short), so the
 * ack window doesn't depend on interrupt latency. The crystal oscillator
 * (needed by the radio) runs only while sending and for HF_HOLD_MS after the
 * last frame.
 *
 * Interference only costs resends: the right half keeps its session and
 * handshakes again only when the left half asks (NEED_HELLO: it restarted
 * and lost the session) or after RESYNC_MS without any ack. Key events are
 * numbered for the whole right-half boot, across handshakes: the left half
 * delivers each one once, also when an ack got lost.
 *
 * Left half (central, USB powered, "PRX"): listens all the time. Valid DATA
 * events are handed to split-timing with their press time on this half's
 * clock (receive time - age - air time) and raised as key position events.
 * Right-half keys still held when the right half restarts or goes silent
 * are released.
 *
 * Security: AES-128-CCM (4-byte MIC) with a pre-shared key built into both
 * halves' firmware, AES in the ECB hardware. Handshake per right-half boot
 * (and again when the left half lost it):
 *   HELLO(session, seq)       right -> left, authenticated (nonce counter =
 *                             seq: the same nonce means the same message);
 *                             seq = the oldest key event not acked yet
 *   HELLO_ACK(session, chal)  left -> right, chal = fresh random per HELLO
 *   DATA(counter, events)     nonce = dir | session | chal | counter,
 *                             counter strictly increasing, events encrypted
 *   ACK(counter, next_seq, leds)  authenticated, LED state encrypted
 *   NEED_HELLO(counter)       left -> right, answer to a DATA frame the left
 *                             half can't open (no session: it restarted)
 * A recorded frame can't be replayed: the left half only accepts the
 * current challenge and increasing counters. A forged ACK would need the
 * key. A replayed HELLO or a forged NEED_HELLO (not authenticated: the left
 * half has no session to authenticate it with) only makes the right half
 * handshake again, like jamming would.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <hal/nrf_clock.h>
#include <hal/nrf_ecb.h>
#include <hal/nrf_radio.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/matrix.h>
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
#include <zmk/backlight.h>
#endif

#include "ccm.h"
#include "split_radio.h"
#if IS_ENABLED(CONFIG_ZMK_SPLIT_TIMING)
#include "split_timing.h"
#endif

LOG_MODULE_REGISTER(split_radio, CONFIG_ZMK_LOG_LEVEL);

#define CENTRAL IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

enum frame_type { HELLO = 1, HELLO_ACK = 2, DATA = 3, ACK = 4, NEED_HELLO = 5 };
enum direction { FROM_RIGHT = 0x52, FROM_LEFT = 0x4c };

#define MAX_PAYLOAD 40
#define MAX_EVENTS 6
#define EVENT_LEN 4 /* position, pressed, age_us (u16) */
#define LED_LEN 6   /* status LEDs: layer, indicators, effect, on; backlight: brightness, on */
#define LINK_TIMEOUT_MS (CONFIG_ZMK_SPLIT_RADIO_POLL_MS * 5 + 500)
#define HELLO_HEADER 7 /* type, session, seq */

/* RAM packet: S0/S1 absent, LENGTH byte, payload */
struct packet {
    uint8_t len;
    uint8_t data[MAX_PAYLOAD];
} __packed;

static struct packet tx_pkt; /* frames this half sends */
static uint8_t key[16];
static uint32_t session, challenge;
static struct split_radio_stats stats;

/* Signal strength of the other half's valid frames and (left half) the
 * channel's noise while only listening, in -dBm, since the last report
 * (key monitor R): tells a weak link (antenna) from a noisy channel. */
struct rssi_acc {
    uint32_t sum, n;
    uint8_t min, max;
};
static struct rssi_acc sig_acc;

static void rssi_add(struct rssi_acc *a, uint8_t v) {
    if (a->n == 0 || v < a->min) {
        a->min = v;
    }
    if (a->n == 0 || v > a->max) {
        a->max = v;
    }
    a->sum += v;
    a->n++;
}

/* " name=-avg(-min..-max)" and reset; strongest first */
static int rssi_text(char *buf, size_t len, const char *name, struct rssi_acc *a) {
    unsigned int lock = irq_lock();
    struct rssi_acc c = *a;
    *a = (struct rssi_acc){0};
    irq_unlock(lock);
    if (c.n == 0) {
        return snprintf(buf, len, " %s=none", name);
    }
    return snprintf(buf, len, " %s=-%u(-%u..-%u)dBm", name, (unsigned)(c.sum / c.n), c.min, c.max);
}
static int64_t last_link_ms = -100000; /* last valid frame from the other half */

/* ---- crypto ------------------------------------------------------------------ */

static void aes_ecb(const uint8_t k[16], const uint8_t in[16], uint8_t out[16]) {
    static struct {
        uint8_t key[16], clear[16], cipher[16];
    } ecb;
    unsigned int lock = irq_lock(); /* thread and ISR both use the ECB */
    memcpy(ecb.key, k, 16);
    memcpy(ecb.clear, in, 16);
    NRF_ECB->ECBDATAPTR = (uint32_t)&ecb;
    NRF_ECB->EVENTS_ENDECB = 0;
    NRF_ECB->EVENTS_ERRORECB = 0;
    NRF_ECB->TASKS_STARTECB = 1;
    while (!NRF_ECB->EVENTS_ENDECB && !NRF_ECB->EVENTS_ERRORECB) {
    }
    memcpy(out, ecb.cipher, 16);
    irq_unlock(lock);
}

static void make_nonce(uint8_t nonce[CCM_NONCE_LEN], uint8_t dir, uint32_t sess, uint32_t chal,
                       uint32_t counter) {
    nonce[0] = dir;
    sys_put_le32(sess, &nonce[1]);
    sys_put_le32(chal, &nonce[5]);
    sys_put_le32(counter, &nonce[9]);
}

/* frame = header (aad) | body (encrypted) | mic */
static uint8_t seal(uint8_t *frame, size_t header_len, size_t body_len, uint8_t dir, uint32_t sess,
                    uint32_t chal, uint32_t counter) {
    uint8_t nonce[CCM_NONCE_LEN];
    make_nonce(nonce, dir, sess, chal, counter);
    ccm_encrypt(aes_ecb, key, nonce, frame, header_len, frame + header_len, body_len,
                frame + header_len + body_len);
    return header_len + body_len + CCM_MIC_LEN;
}

static bool open_frame(uint8_t *frame, size_t len, size_t header_len, uint8_t dir, uint32_t sess,
                       uint32_t chal, uint32_t counter) {
    if (len < header_len + CCM_MIC_LEN) {
        return false;
    }
    uint8_t nonce[CCM_NONCE_LEN];
    size_t body_len = len - header_len - CCM_MIC_LEN;
    make_nonce(nonce, dir, sess, chal, counter);
    return ccm_decrypt(aes_ecb, key, nonce, frame, header_len, frame + header_len, body_len,
                       frame + header_len + body_len);
}

/* Hardware RNG, read directly: without Bluetooth no entropy driver owns it.
 * Bias correction on, a few us per byte. */
static void rand_bytes(void *dst, size_t len) {
    uint8_t *out = dst;
    unsigned int lock = irq_lock();
    NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Msk;
    NRF_RNG->EVENTS_VALRDY = 0;
    NRF_RNG->TASKS_START = 1;
    for (size_t i = 0; i < len; i++) {
        while (!NRF_RNG->EVENTS_VALRDY) {
        }
        NRF_RNG->EVENTS_VALRDY = 0;
        out[i] = NRF_RNG->VALUE;
    }
    NRF_RNG->TASKS_STOP = 1;
    irq_unlock(lock);
}

/* ---- radio ------------------------------------------------------------------- */

static uint32_t now_us(void) { return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks()); }

/* Air time from the end of the age computation to the end of the packet:
 * ramp-up (40 us) + preamble, address, length, payload, CRC at 2 Mbit/s. */
static inline uint32_t air_time_us(uint8_t payload_len) { return 40 + (2 + 5 + 1 + payload_len + 2) * 4; }

/* RSSI sampled at every address match: the signal strength of each frame */
#define SHORTS_BASE                                                                                \
    (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk)

static void radio_configure(void) {
    uint8_t addr_block[16] = "adv360 split adr";
    uint8_t addr[16];
    aes_ecb(key, addr_block, addr); /* addresses differ per key */

    NRF_RADIO->POWER = 1;
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);
    NRF_RADIO->FREQUENCY = CONFIG_ZMK_SPLIT_RADIO_FREQUENCY;
    NRF_RADIO->TXPOWER = (uint32_t)CONFIG_ZMK_SPLIT_RADIO_TX_POWER;
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos) |
                       (RADIO_PCNF0_PLEN_16bit << RADIO_PCNF0_PLEN_Pos);
    NRF_RADIO->PCNF1 = (MAX_PAYLOAD << RADIO_PCNF1_MAXLEN_Pos) | (0 << RADIO_PCNF1_STATLEN_Pos) |
                       (4 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);
    NRF_RADIO->DATAWHITEIV = 0x40 | (addr[6] & 0x3f);
    /* The address bytes sent first must not look like the preamble
     * (0xAA/0x55: the receiver then locks on a bit early or late and the CRC
     * fails): fixed 0xE7 prefix and top base byte like Nordic's ESB, the
     * rest from the key. */
    NRF_RADIO->BASE0 = (sys_get_le32(addr) & 0x00ffffff) | 0xe7000000;
    NRF_RADIO->PREFIX0 = 0xe7;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;
    /* The address isn't part of the CRC: with it included, every frame
     * failed the CRC here although the bytes arrived intact. The address
     * match and the MIC cover it. */
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = 0x11021;
    NRF_RADIO->CRCINIT = 0xffff;
    NRF_RADIO->SHORTS = SHORTS_BASE;
    NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
}

static void radio_start(struct packet *pkt, bool tx) {
    NRF_RADIO->PACKETPTR = (uint32_t)pkt;
    NRF_RADIO->EVENTS_DISABLED = 0;
    if (tx) {
        NRF_RADIO->TASKS_TXEN = 1;
    } else {
        NRF_RADIO->TASKS_RXEN = 1;
    }
}

static bool rx_ok(const struct packet *pkt) {
    return NRF_RADIO->CRCSTATUS == 1 && pkt->len <= MAX_PAYLOAD;
}

/* The radio needs the crystal oscillator. Requested like Zephyr's own
 * Bluetooth controller does (that controller isn't built in): the generic
 * on/off manager never reported "started" here when USB had already started
 * the oscillator. */
#define HF_XTAL_RUNNING (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk)

static void hf_wait_running(void) {
    for (int i = 0; i < 300 && (NRF_CLOCK->HFCLKSTAT & HF_XTAL_RUNNING) != HF_XTAL_RUNNING;
         i++) {
        k_busy_wait(10); /* starts in ~0.4 ms */
    }
}

#if CENTRAL
/* ===== left half: receiver ===================================================== */

static struct packet rx_pkt;

/* Fresh challenge per HELLO, made in the radio interrupt: AES of a random
 * seed (drawn at boot) and a counter. Unique per HELLO, unpredictable
 * without the key, and one ~7 us AES block: the hardware RNG with bias
 * correction took so long here (several 100 us for 4 bytes) that the
 * HELLO_ACK went out after the right half had stopped listening, and most
 * handshakes needed several tries. */
static uint8_t challenge_seed[16];
static uint32_t challenge_count;

static uint32_t new_challenge(void) {
    uint8_t block[16], out[16];
    memcpy(block, challenge_seed, sizeof(block));
    sys_put_le32(++challenge_count, block);
    aes_ecb(key, block, out);
    return sys_get_le32(out);
}

static uint32_t last_counter;
static uint16_t next_seq;
static bool have_session;
static bool sending_ack;
static uint8_t leds[LED_LEN]; /* sent to the right half in every ack */
/* right-half keys pressed (delivered) and not released yet */
static uint32_t held[(ZMK_KEYMAP_LEN + 31) / 32];

#define RIGHT_RESTARTED 0xff /* pseudo event: release everything held */

K_MSGQ_DEFINE(event_msgq, sizeof(struct split_radio_event), 32, 4);

static void release_held_keys(void);

static void raise_events(struct k_work *work) {
    struct split_radio_event ev;
    ARG_UNUSED(work);
    while (k_msgq_get(&event_msgq, &ev, K_NO_WAIT) == 0) {
        if (ev.position == RIGHT_RESTARTED) {
            release_held_keys();
            continue;
        }
        split_radio_deliver(&ev);
    }
}
static K_WORK_DEFINE(raise_work, raise_events);

static void handle_frame(uint32_t rx_us) {
    uint8_t *f = rx_pkt.data;
    uint8_t len = rx_pkt.len;

    if (len >= HELLO_HEADER && f[0] == HELLO) {
        /* header: type, session, seq */
        uint32_t sess = sys_get_le32(&f[1]);
        uint16_t seq = sys_get_le16(&f[5]);
        if (!open_frame(f, len, HELLO_HEADER, FROM_RIGHT, sess, 0, seq)) {
            stats.bad++;
            return;
        }
        if (!have_session || sess != session) {
            if (have_session) {
                /* new right-half boot: keys it held before are gone */
                struct split_radio_event ev = {.position = RIGHT_RESTARTED};
                k_msgq_put(&event_msgq, &ev, K_NO_WAIT);
                k_work_submit(&raise_work);
            }
            next_seq = seq;
        }
        /* else the same right-half boot handshakes again: keep next_seq, so
         * events that arrived but whose ack got lost aren't delivered twice */
        session = sess;
        challenge = new_challenge();
        last_counter = 0;
        have_session = true;
        last_link_ms = k_uptime_get();
        stats.hellos++;
        uint8_t *a = tx_pkt.data;
        a[0] = HELLO_ACK;
        sys_put_le32(session, &a[1]);
        sys_put_le32(challenge, &a[5]);
        tx_pkt.len = seal(a, 9, 0, FROM_LEFT, session, challenge, 0);
        sending_ack = true;
        return;
    }
    if (len >= 5 && f[0] == DATA) {
        /* header: type, counter; body: first_seq (u16), n, events */
        uint32_t counter = sys_get_le32(&f[1]);
        if (have_session && counter <= last_counter) {
            /* old counter: a replay (counters only grow), ignore */
            stats.bad++;
            return;
        }
        if (!have_session ||
            !open_frame(f, len, 5, FROM_RIGHT, session, challenge, counter)) {
            /* this half restarted (no session) or the right half uses a
             * challenge this half no longer has: ask for a handshake */
            stats.bad++;
            uint8_t *a = tx_pkt.data;
            a[0] = NEED_HELLO;
            sys_put_le32(counter, &a[1]);
            tx_pkt.len = 5;
            sending_ack = true;
            return;
        }
        last_counter = counter;
        last_link_ms = k_uptime_get();
        uint8_t *b = &f[5];
        uint16_t first = sys_get_le16(b);
        uint8_t n = b[2];
        uint32_t air = air_time_us(len);
        bool any = false;
        for (uint8_t i = 0; i < n && 3 + (i + 1) * EVENT_LEN <= len - 5 - CCM_MIC_LEN; i++) {
            uint16_t seq = first + i;
            if ((int16_t)(seq - next_seq) < 0) {
                continue; /* delivered already (its ack got lost) */
            }
            const uint8_t *e = &b[3 + i * EVENT_LEN];
            struct split_radio_event ev = {
                .position = e[0],
                .pressed = e[1] & 1,
                .press_us = rx_us - air - sys_get_le16(&e[2]),
            };
            if (ev.position >= ZMK_KEYMAP_LEN) {
                next_seq = seq + 1;
                continue;
            }
            if (k_msgq_put(&event_msgq, &ev, K_NO_WAIT) == 0) {
                next_seq = seq + 1;
                any = true;
            }
        }
        stats.frames++;
        if (any) {
            k_work_submit(&raise_work);
        }
        uint8_t *a = tx_pkt.data;
        a[0] = ACK;
        sys_put_le32(counter, &a[1]);
        sys_put_le16(next_seq, &a[5]);
        memcpy(&a[7], leds, LED_LEN);
        tx_pkt.len = seal(a, 7, LED_LEN, FROM_LEFT, session, challenge, counter);
        sending_ack = true;
    }
}

static void radio_isr(const void *arg) {
    ARG_UNUSED(arg);
    if (!NRF_RADIO->EVENTS_DISABLED) {
        return;
    }
    NRF_RADIO->EVENTS_DISABLED = 0;
    if (sending_ack) { /* the ack went out: listen again */
        sending_ack = false;
        radio_start(&rx_pkt, false);
        return;
    }
    uint32_t rx_us = now_us();
    uint32_t rx_cycles = k_cycle_get_32();
    if (rx_ok(&rx_pkt)) {
        rssi_add(&sig_acc, NRF_RADIO->RSSISAMPLE);
        handle_frame(rx_us);
    } else {
        stats.crc_errors++;
    }
    if (sending_ack) {
        /* the right half switches to receive in its interrupt handler:
         * give it time before the ack starts */
        uint32_t wait = k_us_to_cyc_ceil32(CONFIG_ZMK_SPLIT_RADIO_ACK_DELAY_US);
        while (k_cycle_get_32() - rx_cycles < wait) {
        }
    }
    radio_start(sending_ack ? &tx_pkt : &rx_pkt, sending_ack);
}

/* system work queue */
void split_radio_deliver(const struct split_radio_event *ev) {
    if (ev->pressed) {
        held[ev->position / 32] |= BIT(ev->position % 32);
    } else {
        held[ev->position / 32] &= ~BIT(ev->position % 32);
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_TIMING)
    split_timing_remote_time(ev->position, ev->pressed, ev->press_us);
#endif
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = 0,
        .position = ev->position,
        .state = ev->pressed,
        .timestamp = k_uptime_get(),
    });
}

/* system work queue */
static void release_held_keys(void) {
    for (int p = 0; p < ZMK_KEYMAP_LEN; p++) {
        if (held[p / 32] & BIT(p % 32)) {
            LOG_INF("releasing right-half key %d", p);
            struct split_radio_event ev = {.position = p, .pressed = false, .press_us = now_us()};
            split_radio_deliver(&ev);
        }
    }
}

/* the right half went silent (battery out, out of range): no stuck keys */
static void link_check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(link_work, link_check);
static void link_check(struct k_work *work) {
    if (k_uptime_get() - last_link_ms > LINK_TIMEOUT_MS) {
        release_held_keys();
    }
    k_work_reschedule(&link_work, K_MSEC(250));
}

/* LEDs: Kinesis' rgb_underglow.c sends the right half's LED state with
 * this (Bluetooth: zmk/split/bluetooth/central.c); it goes out in every ack. */
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
int zmk_split_bt_update_led(struct zmk_periph_led *periph) {
    uint8_t l[4] = {periph->layer, periph->indicators, periph->effect, periph->on};
    unsigned int lock = irq_lock();
    memcpy(leds, l, 4);
    irq_unlock(lock);
    return 0;
}
#endif

/* key backlight, same (Kinesis' backlight.c) */
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
int zmk_split_bt_update_bl(struct backlight_state *bl) {
    unsigned int lock = irq_lock();
    leds[4] = bl->brightness;
    leds[5] = bl->on;
    irq_unlock(lock);
    return 0;
}
#endif

/* The left LED that showed the Bluetooth profile shows the link to the
 * right half instead: steady white = linked, blinking = no frames from it. */
int zmk_ble_active_profile_index(void) { return 0; }
bool zmk_ble_active_profile_is_open(void) { return false; }
bool zmk_ble_active_profile_is_connected(void) {
    return k_uptime_get() - last_link_ms < LINK_TIMEOUT_MS;
}

/* channel noise: sample the RSSI while the receiver waits for a frame */
static struct rssi_acc noise_acc;

static void noise_sample(struct k_timer *t) {
    if ((NRF_RADIO->STATE & RADIO_STATE_STATE_Msk) != RADIO_STATE_STATE_Rx) {
        return;
    }
    NRF_RADIO->EVENTS_RSSIEND = 0;
    NRF_RADIO->TASKS_RSSISTART = 1;
    for (int i = 0; i < 100 && !NRF_RADIO->EVENTS_RSSIEND; i++) {
    }
    if (NRF_RADIO->EVENTS_RSSIEND) {
        rssi_add(&noise_acc, NRF_RADIO->RSSISAMPLE);
    }
}
K_TIMER_DEFINE(noise_timer, noise_sample, NULL);

static void role_start(void) {
    /* USB powered: keep the crystal and the receiver on */
    z_nrf_clock_bt_ctlr_hf_request();
    hf_wait_running();
    radio_start(&rx_pkt, false);
    k_work_reschedule(&link_work, K_MSEC(1000));
    if (IS_ENABLED(CONFIG_ZMK_KEY_MONITOR)) {
        /* test builds only: only the key monitor (R) reports it */
        k_timer_start(&noise_timer, K_MSEC(5), K_MSEC(5));
    }
}

#else
/* ===== right half: sender ====================================================== */

/* The ack is received into the frame's own buffer: the radio then goes from
 * sending to receiving by itself (DISABLED -> RXEN short) without a new
 * packet pointer, so catching the ack doesn't depend on how soon this half's
 * interrupt handler runs (USB traffic on this half delayed it). */
#define ack_pkt tx_pkt

struct pending_event {
    uint16_t seq;
    uint8_t position;
    bool pressed;
    uint32_t press_us;
};

static struct pending_event queue[64];
static uint8_t q_head, q_len; /* ring */
static uint16_t next_seq;     /* seq of the next queued event */
static uint32_t counter;
static bool have_challenge;
static bool poll_due;        /* send a frame even without key events */
static uint8_t frame_events; /* events in the frame in flight */
static uint32_t frame_counter;
static uint32_t frame_built_us;
static int retries;
static uint8_t leds[LED_LEN];
static bool leds_valid;

enum { IDLE, CLOCK_WAIT, TX, WAIT_ACK, BACKOFF } state;

static void ack_timeout(struct k_timer *t);
static void backoff_done(struct k_timer *t);
K_TIMER_DEFINE(ack_timer, ack_timeout, NULL);
K_TIMER_DEFINE(backoff_timer, backoff_done, NULL);

/* -- crystal on demand: on while sending and HF_HOLD_MS after -- */
static bool hf_on;
static void send_next(void);

static void hf_release_fn(struct k_work *work) {
    unsigned int lock = irq_lock();
    if (hf_on && (state == IDLE || state == BACKOFF)) {
        hf_on = false;
        z_nrf_clock_bt_ctlr_hf_release();
    }
    irq_unlock(lock);
}
static K_WORK_DELAYABLE_DEFINE(hf_release_work, hf_release_fn);

/* thread context: make sure the crystal runs, then send */
static void kick(void) {
    unsigned int lock = irq_lock();
    if (state != IDLE) {
        irq_unlock(lock); /* busy: the queue / poll flag is picked up after */
        return;
    }
    state = CLOCK_WAIT;
    k_work_cancel_delayable(&hf_release_work);
    if (!hf_on) {
        hf_on = true;
        z_nrf_clock_bt_ctlr_hf_request();
    }
    irq_unlock(lock);
    hf_wait_running();
    lock = irq_lock();
    if (state == CLOCK_WAIT) {
        send_next();
    }
    irq_unlock(lock);
}

static void build_frame(void) {
    uint8_t *f = tx_pkt.data;
    if (!have_challenge) {
        /* the oldest event not acked yet: where the left half continues
         * when it has no session (it restarted, or this half did) */
        uint16_t seq = q_len ? queue[q_head].seq : next_seq;
        f[0] = HELLO;
        sys_put_le32(session, &f[1]);
        sys_put_le16(seq, &f[5]);
        tx_pkt.len = seal(f, HELLO_HEADER, 0, FROM_RIGHT, session, 0, seq);
        frame_events = 0;
        return;
    }
    frame_counter = ++counter;
    f[0] = DATA;
    sys_put_le32(frame_counter, &f[1]);
    uint8_t n = MIN(q_len, MAX_EVENTS);
    uint8_t *b = &f[5];
    uint32_t now = now_us();
    sys_put_le16(n ? queue[q_head].seq : next_seq, b);
    b[2] = n;
    for (uint8_t i = 0; i < n; i++) {
        const struct pending_event *e = &queue[(q_head + i) % ARRAY_SIZE(queue)];
        uint8_t *o = &b[3 + i * EVENT_LEN];
        o[0] = e->position;
        o[1] = e->pressed;
        sys_put_le16(MIN(now - e->press_us, UINT16_MAX), &o[2]);
    }
    frame_events = n;
    frame_built_us = now;
    tx_pkt.len = seal(f, 5, 3 + n * EVENT_LEN, FROM_RIGHT, session, challenge, frame_counter);
}

/* call with interrupts locked */
static void send_next(void) {
    if (have_challenge && q_len == 0 && !poll_due) {
        state = IDLE;
        k_work_reschedule(&hf_release_work, K_MSEC(CONFIG_ZMK_SPLIT_RADIO_HF_HOLD_MS));
        return;
    }
    build_frame();
    state = TX;
    NRF_RADIO->SHORTS = SHORTS_BASE | RADIO_SHORTS_DISABLED_RXEN_Msk;
    radio_start(&tx_pkt, true);
}

static void led_apply(struct k_work *work) {
    uint8_t l[LED_LEN];
    unsigned int lock = irq_lock();
    memcpy(l, leds, LED_LEN);
    irq_unlock(lock);
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    zmk_rgb_underglow_set_periph(
        (struct zmk_periph_led){.layer = l[0], .indicators = l[1], .effect = l[2], .on = l[3]});
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
    zmk_backlight_update_vals((struct backlight_state){.brightness = l[4], .on = l[5]});
#endif
}
static K_WORK_DEFINE(led_work, led_apply);

/* handshake done: drop key presses that waited too long for the link (keep
 * releases). Their numbers stay: the left half skips what it already has. */
static void drop_stale_presses(void) {
    uint32_t now = now_us();
    uint8_t n = 0;
    for (uint8_t i = 0; i < q_len; i++) {
        struct pending_event e = queue[(q_head + i) % ARRAY_SIZE(queue)];
        if (!e.pressed || now - e.press_us < CONFIG_ZMK_SPLIT_RADIO_STALE_MS * 1000) {
            queue[(q_head + n) % ARRAY_SIZE(queue)] = e; /* n <= i: safe in place */
            n++;
        }
    }
    q_len = n;
}

static void handle_ack(void) {
    uint8_t *a = ack_pkt.data;
    uint8_t len = ack_pkt.len;
    if (!have_challenge && len >= 9 && a[0] == HELLO_ACK && sys_get_le32(&a[1]) == session) {
        uint32_t chal = sys_get_le32(&a[5]);
        if (!open_frame(a, len, 9, FROM_LEFT, session, chal, 0)) {
            return;
        }
        challenge = chal;
        have_challenge = true;
        counter = 0;
        poll_due = true; /* get the LED state at once */
        drop_stale_presses();
        stats.hellos++;
    } else if (have_challenge && len >= 7 + LED_LEN + CCM_MIC_LEN && a[0] == ACK &&
               sys_get_le32(&a[1]) == frame_counter) {
        if (!open_frame(a, len, 7, FROM_LEFT, session, challenge, frame_counter)) {
            return;
        }
        uint16_t acked_next = sys_get_le16(&a[5]);
        while (q_len > 0 && (int16_t)(queue[q_head].seq - acked_next) < 0) {
            q_head = (q_head + 1) % ARRAY_SIZE(queue);
            q_len--;
        }
        if (frame_events == 0) {
            poll_due = false;
        }
        if (!leds_valid || memcmp(leds, &a[7], LED_LEN) != 0) {
            memcpy(leds, &a[7], LED_LEN);
            leds_valid = true;
            k_work_submit(&led_work);
        }
        stats.frames++;
        stats.last_rtt_us = now_us() - frame_built_us;
    } else if (have_challenge && len >= 5 && a[0] == NEED_HELLO &&
               sys_get_le32(&a[1]) == frame_counter) {
        /* the left half lost the session (it restarted): handshake at once */
        have_challenge = false;
        stats.resyncs++;
        k_timer_stop(&ack_timer);
        retries = 0;
        send_next();
        return;
    } else {
        return; /* not ours: keep waiting */
    }
    last_link_ms = k_uptime_get();
    k_timer_stop(&ack_timer);
    retries = 0;
    send_next();
}

static void radio_isr(const void *arg) {
    ARG_UNUSED(arg);
    if (!NRF_RADIO->EVENTS_DISABLED) {
        return;
    }
    NRF_RADIO->EVENTS_DISABLED = 0;
    if (state == TX) {
        /* sent, and the short already started the receiver for the ack:
         * no restart after it, this handler looks at what arrives */
        NRF_RADIO->SHORTS = SHORTS_BASE;
        state = WAIT_ACK;
        k_timer_start(&ack_timer, K_USEC(CONFIG_ZMK_SPLIT_RADIO_ACK_TIMEOUT_US), K_NO_WAIT);
    } else if (state == WAIT_ACK) {
        if (rx_ok(&ack_pkt)) {
            rssi_add(&sig_acc, NRF_RADIO->RSSISAMPLE);
            handle_ack();
        } else {
            stats.crc_errors++;
        }
        if (state == WAIT_ACK) {
            radio_start(&ack_pkt, false); /* keep listening until the timeout */
        }
    }
}

static void retry_fn(struct k_work *work) {
    unsigned int lock = irq_lock();
    if (state == BACKOFF) {
        state = IDLE;
    }
    irq_unlock(lock);
    kick();
}
static K_WORK_DEFINE(retry_work, retry_fn);

static void ack_timeout(struct k_timer *t) {
    unsigned int lock = irq_lock();
    if (state == WAIT_ACK) {
        state = BACKOFF;
        NRF_RADIO->TASKS_DISABLE = 1;
        stats.retries++;
        retries++;
        if (have_challenge &&
            k_uptime_get() - last_link_ms > CONFIG_ZMK_SPLIT_RADIO_RESYNC_MS) {
            /* no ack for long: the left half may have restarted without its
             * NEED_HELLO getting through. Interference alone only costs
             * resends (a handshake used to follow 10 lost frames, ~10 ms of
             * noise, and showed the red "disconnected" LEDs). */
            have_challenge = false;
            stats.resyncs++;
        }
        static uint32_t x; /* xorshift: backoff jitter only */
        x = x ? x : session | 1;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        /* quick retries first (interference); while the left half doesn't
         * answer (restarting: ~1 s) every 20-30 ms; after 5 s without it
         * (unplugged) slower and slower, up to 1 s: save the battery. A key
         * press on this half retries at once (key_listener). */
        uint32_t delay_us = 50 + x % 300;
        if (retries > 20) {
            int64_t outage_ms = k_uptime_get() - last_link_ms;
            delay_us = outage_ms < 5000 ? 20000 + x % 10000
                                        : MIN(20000u << MIN((outage_ms - 5000) / 2000, 6),
                                              1000000u) + x % 10000;
            k_work_reschedule(&hf_release_work, K_NO_WAIT);
        }
        k_timer_start(&backoff_timer, K_USEC(delay_us), K_NO_WAIT);
    }
    irq_unlock(lock);
}

static void backoff_done(struct k_timer *t) { k_work_submit(&retry_work); }

static void poll_fn(struct k_work *work) {
    unsigned int lock = irq_lock();
    poll_due = true;
    irq_unlock(lock);
    kick();
}
static K_WORK_DEFINE(poll_work, poll_fn);
static void poll_timer_fn(struct k_timer *t) { k_work_submit(&poll_work); }
K_TIMER_DEFINE(poll_timer, poll_timer_fn, NULL);

static int key_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos == NULL || pos->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    unsigned int lock = irq_lock();
    if (q_len < ARRAY_SIZE(queue)) {
        queue[(q_head + q_len) % ARRAY_SIZE(queue)] = (struct pending_event){
            .seq = next_seq++,
            .position = pos->position,
            .pressed = pos->state,
            .press_us = now_us(),
        };
        q_len++;
    }
    bool waiting = state == BACKOFF;
    irq_unlock(lock);
    if (waiting) {
        /* the link is down: try again now, someone is typing */
        k_timer_stop(&backoff_timer);
        retry_fn(NULL);
    } else {
        kick();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(split_radio, key_listener);
ZMK_SUBSCRIPTION(split_radio, zmk_position_state_changed);

/* Kinesis' rgb_underglow.c blinks the right half's LEDs red while it isn't
 * connected to the left half (Bluetooth: zmk/split/bluetooth/peripheral.c) */
bool zmk_split_bt_peripheral_is_bonded(void) { return true; }
bool zmk_split_bt_peripheral_is_connected(void) {
    return have_challenge && k_uptime_get() - last_link_ms < LINK_TIMEOUT_MS;
}

static void role_start(void) {
    k_timer_start(&poll_timer, K_MSEC(CONFIG_ZMK_SPLIT_RADIO_POLL_MS),
                  K_MSEC(CONFIG_ZMK_SPLIT_RADIO_POLL_MS));
    kick(); /* HELLO */
}
#endif

const struct split_radio_stats *split_radio_get_stats(void) { return &stats; }

static int debug_text(char *buf, size_t len) {
#if CENTRAL
    int n = rssi_text(buf, len, "sig", &sig_acc);
    n += rssi_text(buf + n, len - n, "noise", &noise_acc);
    return n + snprintf(buf + n, len - n, " leds=%02x%02x%02x%02x bl=%u/%u", leds[0], leds[1],
                        leds[2], leds[3], leds[4], leds[5]);
#else
    int n = rssi_text(buf, len, "sig", &sig_acc);
    return n + snprintf(buf + n, len - n, " state=%d hf_on=%d q=%d leds=%02x%02x%02x%02x bl=%u/%u",
                        state, hf_on, q_len, leds[0], leds[1], leds[2], leds[3], leds[4], leds[5]);
#endif
}

int split_radio_stats_text(char *buf, size_t len) {
    int n = snprintf(buf, len,
                     "frames=%u retries=%u crc=%u bad=%u hellos=%u resyncs=%u rtt=%u hfclk=%x "
                     "link_ms=%d",
                     stats.frames, stats.retries, stats.crc_errors, stats.bad, stats.hellos,
                     stats.resyncs, stats.last_rtt_us, (unsigned)NRF_CLOCK->HFCLKSTAT,
                     (int)(k_uptime_get() - last_link_ms));
    return n + debug_text(buf + n, len - n);
}

/* ---- init -------------------------------------------------------------------- */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

static int split_radio_init(void) {
    const char *hex = CONFIG_ZMK_SPLIT_RADIO_KEY;
    if (strlen(hex) != 32) {
        LOG_ERR("CONFIG_ZMK_SPLIT_RADIO_KEY must be 32 hex digits");
        return -EINVAL;
    }
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -EINVAL;
        }
        key[i] = (hi << 4) | lo;
    }
    rand_bytes(&session, sizeof(session));
    rand_bytes(&challenge, sizeof(challenge));
#if CENTRAL
    rand_bytes(challenge_seed, sizeof(challenge_seed));
#endif

    radio_configure();
    IRQ_CONNECT(RADIO_IRQn, 1, radio_isr, NULL, 0);
    irq_enable(RADIO_IRQn);
    role_start();
    LOG_INF("split radio up (%s)", CENTRAL ? "receiver" : "sender");
    return 0;
}

SYS_INIT(split_radio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
