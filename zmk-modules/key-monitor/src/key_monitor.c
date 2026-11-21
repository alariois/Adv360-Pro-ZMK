/*
 * Key monitor: streams key positions and layer state over a dedicated USB
 * serial port, and (optionally) accepts injected key presses.
 *
 * TEST BUILDS ONLY - see Kconfig for the protocol and the security caveats.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#if IS_ENABLED(CONFIG_BT)
#include <zephyr/bluetooth/conn.h>
#endif
#include <zmk/matrix.h>

/* The peripheral (right half) has no keymap, layers or HID: there the
 * monitor only reports and injects its own key positions, which then
 * travel to the central over the split link like real key presses. */
#define CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if CENTRAL
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif
#endif

LOG_MODULE_REGISTER(key_monitor, CONFIG_ZMK_LOG_LEVEL);

#define KEY_MONITOR_VERSION 2

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_key_monitor_uart),
             "key-monitor needs a chosen zmk,key-monitor-uart (use the key-monitor snippet)");

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zmk_key_monitor_uart));

/* Serializes whole lines from the different threads that raise events. */
static struct k_spinlock tx_lock;

static bool host_connected(void) {
    uint32_t dtr = 0;
    return uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr;
}

/* Non-blocking: CDC ACM poll_out queues into a ring buffer (dropping the
 * oldest data when full) and flushes from the USB work queue. */
static void send_line(const char *line) {
    if (!host_connected()) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    for (const char *c = line; *c; c++) {
        uart_poll_out(uart, *c);
    }
    k_spin_unlock(&tx_lock, key);
}

static void send_layers(void) {
#if CENTRAL
    char buf[40];
    snprintf(buf, sizeof(buf), "L %x %u\n", (unsigned int)zmk_keymap_layer_state(),
             (unsigned int)zmk_keymap_highest_layer_active());
    send_line(buf);
#endif
}

/* the watchdog module keeps the reset reason (nRF RESETREAS, 2 = watchdog) */
__weak uint32_t zmk_reset_reason(void) { return 0; }
__weak int zmk_prev_crumbs(uint32_t *out, int max) { return 0; }

static void send_hello(void) {
    char buf[96];
    snprintf(buf, sizeof(buf), "H key-monitor %d inject=%d keys=%d role=%s reset=%x\n",
             KEY_MONITOR_VERSION, IS_ENABLED(CONFIG_ZMK_KEY_MONITOR_INJECT), ZMK_KEYMAP_LEN,
             CENTRAL ? "central" : "peripheral", (unsigned int)zmk_reset_reason());
    send_line(buf);
    send_layers();
}

static int key_monitor_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        char buf[40];
        /* uptime in us: when this half saw the key (split latency) */
        snprintf(buf, sizeof(buf), "P %u %u %u\n", (unsigned int)pos->position, pos->state ? 1 : 0,
                 (unsigned int)k_ticks_to_us_floor64(k_uptime_ticks()));
        send_line(buf);
    }
#if CENTRAL
    else if (as_zmk_layer_state_changed(eh)) {
        send_layers();
    }
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_monitor, key_monitor_listener);
ZMK_SUBSCRIPTION(key_monitor, zmk_position_state_changed);
#if CENTRAL
ZMK_SUBSCRIPTION(key_monitor, zmk_layer_state_changed);
#endif

/* ---- host -> device ---------------------------------------------------- */

struct command {
    char type; /* 'K' inject, '?' hello, 'I' indicators, 'B' Bluetooth links */
    uint8_t state;
    uint16_t position;
};

static void send_links(void);

/* split-radio counters, when that module is built in */
__weak int split_radio_stats_text(char *buf, size_t len) {
    ARG_UNUSED(len);
    buf[0] = '\0';
    return 0;
}

/* split-timing statistics, when that module is built in (central) */
__weak int split_timing_stats(char *buf, size_t len, bool reset) {
    ARG_UNUSED(len);
    ARG_UNUSED(reset);
    buf[0] = '\0';
    return 0;
}

K_MSGQ_DEFINE(command_queue, sizeof(struct command), 32, 4);

static void process_commands(struct k_work *work);
static K_WORK_DEFINE(command_work, process_commands);

static void handle_line(char *line) {
    struct command cmd = {.type = line[0]};

    switch (line[0]) {
    case '?':
    case 'I':
    case 'B':
    case 'S':
    case 'R':
    case 'X':
    case 'C':
        break;
    case 'K': {
        char *end;
        unsigned long position = strtoul(line + 1, &end, 10);
        unsigned long state = strtoul(end, &end, 10);
        if (position >= ZMK_KEYMAP_LEN || state > 1) {
            send_line("E bad K command\n");
            return;
        }
        cmd.position = position;
        cmd.state = state;
        break;
    }
    default:
        send_line("E unknown command\n");
        return;
    }

    if (k_msgq_put(&command_queue, &cmd, K_NO_WAIT) != 0) {
        send_line("E queue full\n");
        return;
    }
    k_work_submit(&command_work);
}

/* Runs from the CDC ACM driver's work queue: only collect lines here. */
static void uart_callback(const struct device *dev, void *user_data) {
    static char line[32];
    static size_t len;
    uint8_t buf[16];

    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        int n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (len > 0) {
                    line[len] = '\0';
                    handle_line(line);
                    len = 0;
                }
            } else if (len < sizeof(line) - 1) {
                line[len++] = buf[i];
            }
        }
    }
}

/*
 * Commands are processed on the system work queue, exactly like ZMK handles
 * real key presses (physical_layouts.c): raising a position event runs the
 * whole keymap/behavior/HID pipeline, which needs that queue's larger stack.
 */
static void process_commands(struct k_work *work) {
    struct command cmd;

    ARG_UNUSED(work);
    while (k_msgq_get(&command_queue, &cmd, K_NO_WAIT) == 0) {
        if (cmd.type == '?') {
            send_hello();
        } else if (cmd.type == 'I') {
            char buf[16];
#if CENTRAL && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
            /* What the LEDs show (caps lock includes smart-keys caps word) */
            snprintf(buf, sizeof(buf), "I %x\n", zmk_hid_indicators_get_current_profile());
#else
            snprintf(buf, sizeof(buf), "I 0\n");
#endif
            send_line(buf);
        } else if (cmd.type == 'C') {
            uint32_t c[32];
            int n = zmk_prev_crumbs(c, ARRAY_SIZE(c));
            char buf[24];
            send_line("C");
            for (int i = 0; i < n; i++) {
                snprintf(buf, sizeof(buf), " %u@%u", (unsigned)(c[i] >> 24),
                         (unsigned)(c[i] & 0xffffff));
                send_line(buf);
            }
            send_line("\n");
        } else if (cmd.type == 'X') {
            sys_reboot(SYS_REBOOT_COLD);
        } else if (cmd.type == 'B') {
            send_links();
        } else if (cmd.type == 'R') {
            static char rbuf[240];
            rbuf[0] = 'R';
            rbuf[1] = ' ';
            int n = split_radio_stats_text(rbuf + 2, sizeof(rbuf) - 4);
            rbuf[2 + n] = '\n';
            rbuf[3 + n] = '\0';
            send_line(rbuf);
        } else if (cmd.type == 'S') {
            static char buf[200];
            buf[0] = 'S';
            buf[1] = ' ';
            int n = split_timing_stats(buf + 2, sizeof(buf) - 4, true);
            buf[2 + n] = '\n';
            buf[3 + n] = '\0';
            send_line(buf);
        } else if (cmd.type == 'K') {
#if IS_ENABLED(CONFIG_ZMK_KEY_MONITOR_INJECT)
            raise_zmk_position_state_changed((struct zmk_position_state_changed){
                .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
                .position = cmd.position,
                .state = cmd.state,
                .timestamp = k_uptime_get(),
            });
#else
            send_line("E injection disabled\n");
#endif
        }
    }
}

#if IS_ENABLED(CONFIG_BT)
static void send_conn_info(struct bt_conn *conn, void *data) {
    struct bt_conn_info info;
    char buf[80];
    ARG_UNUSED(data);
    if (bt_conn_get_info(conn, &info) != 0 || info.type != BT_CONN_TYPE_LE) {
        return;
    }
    /* interval in 1.25 ms units, timeout in 10 ms units */
    snprintf(buf, sizeof(buf), "B %s %u %u %u %u %u\n",
             info.role == BT_CONN_ROLE_CENTRAL ? "central" : "peripheral", info.le.interval,
             info.le.latency, info.le.timeout, info.le.phy ? info.le.phy->tx_phy : 0,
             info.le.phy ? info.le.phy->rx_phy : 0);
    send_line(buf);
}
#endif

static void send_links(void) {
#if IS_ENABLED(CONFIG_BT)
    bt_conn_foreach(BT_CONN_TYPE_LE, send_conn_info, NULL);
#endif
    send_line("B end\n");
}

/* Notice a host opening the port (DTR raised) and greet it. */
static void poll_host(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(host_work, poll_host);

static void poll_host(struct k_work *work) {
    static bool was_connected;
    bool connected = host_connected();

    ARG_UNUSED(work);
    if (connected && !was_connected) {
        send_hello();
    }
    was_connected = connected;
    k_work_schedule(&host_work, K_MSEC(100));
}

static int key_monitor_init(void) {
    if (!device_is_ready(uart)) {
        LOG_ERR("Key monitor UART not ready");
        return -ENODEV;
    }
    uart_irq_callback_set(uart, uart_callback);
    uart_irq_rx_enable(uart);
    k_work_schedule(&host_work, K_MSEC(100));
    LOG_WRN("Key monitor enabled: key presses are readable%s over USB serial",
            IS_ENABLED(CONFIG_ZMK_KEY_MONITOR_INJECT) ? " and injectable" : "");
    return 0;
}

SYS_INIT(key_monitor_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
