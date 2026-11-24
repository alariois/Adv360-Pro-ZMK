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
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

/* Before any ZMK header: dt-bindings/zmk/keys.h defines a macro P (the P
 * key), which breaks nrf_gpio.h's NRF_##P##0 token pasting. */
#if IS_ENABLED(CONFIG_SOC_SERIES_NRF52X)
#include <hal/nrf_gpio.h>
#endif

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
    char type; /* 'K' inject, '?' hello, 'I' indicators, 'B' Bluetooth links, 'M' matrix */
    uint8_t state;
    uint16_t position;
    uint16_t wait_us; /* 'M': settle time before reading the rows */
};

static void send_links(void);

/* ---- raw matrix probe ('M') ----------------------------------------------
 *
 * Scans this half's key matrix directly on the GPIO registers, bypassing
 * ZMK's kscan driver, to tell hardware faults apart (docs/keyfix.md): for
 * each column, drive only that column active, wait, read all rows. Also
 * reads every column's pad back (input buffer connected for a moment), which
 * shows a pin that can't actually reach the level it is driven to, and tries
 * it with the high drive and as an input with a pull-up.
 *
 * Runs on the system work queue like ZMK's own scan (never in the middle of
 * one) with interrupts locked per column (~50 us + the wait), and leaves the
 * column pins exactly as it found them. A row interrupt that this raises
 * only makes ZMK scan once more.
 */
#define KSCAN_NODE DT_CHOSEN(zmk_kscan)
#define MATRIX_PROBE                                                                               \
    (IS_ENABLED(CONFIG_SOC_SERIES_NRF52X) && DT_HAS_CHOSEN(zmk_kscan) &&                           \
     DT_NODE_HAS_COMPAT(KSCAN_NODE, zmk_kscan_gpio_matrix))

#if MATRIX_PROBE
/* col2row: the columns are the outputs, the rows the inputs */
BUILD_ASSERT(DT_ENUM_IDX(KSCAN_NODE, diode_direction) == 1, "matrix probe: col2row only");

struct probe_pin {
    uint32_t pin; /* nRF absolute pin number (port * 32 + pin) */
    bool active_low;
};

#define PROBE_PIN(node, prop, idx)                                                                 \
    {.pin = NRF_GPIO_PIN_MAP(DT_PROP(DT_GPIO_CTLR_BY_IDX(node, prop, idx), port),                  \
                             DT_GPIO_PIN_BY_IDX(node, prop, idx)),                                  \
     .active_low = (DT_GPIO_FLAGS_BY_IDX(node, prop, idx) & GPIO_ACTIVE_LOW) != 0},

static const struct probe_pin probe_cols[] = {DT_FOREACH_PROP_ELEM(KSCAN_NODE, col_gpios, PROBE_PIN)};
static const struct probe_pin probe_rows[] = {DT_FOREACH_PROP_ELEM(KSCAN_NODE, row_gpios, PROBE_PIN)};

/* Time for the rows to fall back (pull-downs) after all columns go inactive. */
#define PROBE_SETTLE_US 20

static bool probe_level(const struct probe_pin *p) { return nrf_gpio_pin_read(p->pin) != p->active_low; }

static bool probe_driven(const struct probe_pin *p) {
    return nrf_gpio_pin_out_read(p->pin) != p->active_low;
}

static void probe_drive(const struct probe_pin *p, bool active) {
    nrf_gpio_pin_write(p->pin, active != p->active_low);
}

static uint32_t probe_rows_now(void) {
    uint32_t bits = 0;
    for (size_t r = 0; r < ARRAY_SIZE(probe_rows); r++) {
        if (probe_level(&probe_rows[r])) {
            bits |= BIT(r);
        }
    }
    return bits;
}

static void matrix_probe(uint32_t wait_us) {
    const size_t ncols = ARRAY_SIZE(probe_cols);
    uint32_t out = 0, idle, drv = 0, rel = 0, hd = 0, pu = 0, rows[ARRAY_SIZE(probe_cols)];

    /* as found: idle ZMK drives all columns, so `idle` shows a held key's
     * row whenever its path to the chip is intact */
    unsigned int key = irq_lock();
    for (size_t c = 0; c < ncols; c++) {
        if (probe_driven(&probe_cols[c])) {
            out |= BIT(c);
        }
    }
    idle = probe_rows_now();
    irq_unlock(key);

    for (size_t c = 0; c < ncols; c++) {
        uint32_t pin = probe_cols[c].pin;
        NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&pin); /* pin: now within the port */

        key = irq_lock();
        uint32_t cnf = port->PIN_CNF[pin];
        port->PIN_CNF[pin] = cnf & ~GPIO_PIN_CNF_INPUT_Msk; /* INPUT_Connect: read the pad */
        for (size_t i = 0; i < ncols; i++) {
            probe_drive(&probe_cols[i], false);
        }
        k_busy_wait(PROBE_SETTLE_US);
        if (probe_level(&probe_cols[c])) {
            rel |= BIT(c); /* high although driven inactive */
        }
        probe_drive(&probe_cols[c], true);
        k_busy_wait(wait_us);
        rows[c] = probe_rows_now();
        k_busy_wait(1);
        if (probe_level(&probe_cols[c])) {
            drv |= BIT(c); /* reached the active level */
        }
        /* a pad that can't: is it held by something outside (it stays there
         * with the high drive and with a mere pull-up) or is the pin's own
         * output dead (a pull-up still gets it high)? */
        port->PIN_CNF[pin] = (cnf & ~(GPIO_PIN_CNF_INPUT_Msk | GPIO_PIN_CNF_DRIVE_Msk)) |
                             (GPIO_PIN_CNF_DRIVE_H0H1 << GPIO_PIN_CNF_DRIVE_Pos);
        k_busy_wait(5);
        if (probe_level(&probe_cols[c])) {
            hd |= BIT(c);
        }
        port->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
                             (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
                             (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos);
        k_busy_wait(PROBE_SETTLE_US);
        if (nrf_gpio_pin_read(probe_cols[c].pin)) {
            pu |= BIT(c); /* physical level */
        }
        for (size_t i = 0; i < ncols; i++) {
            probe_drive(&probe_cols[i], out & BIT(i));
        }
        port->PIN_CNF[pin] = cnf;
        irq_unlock(key);
    }

    static char buf[200];
    int n = snprintf(buf, sizeof(buf), "M us=%u out=%x idle=%x", wait_us, out, idle);
    for (size_t c = 0; c < ncols; c++) {
        n += snprintf(buf + n, sizeof(buf) - n, " c%u=%x", (unsigned int)c, rows[c]);
    }
    snprintf(buf + n, sizeof(buf) - n, " drv=%x rel=%x hd=%x pu=%x\n", drv, rel, hd, pu);
    send_line(buf);
}
#else
static void matrix_probe(uint32_t wait_us) {
    ARG_UNUSED(wait_us);
    send_line("E no GPIO matrix to probe\n");
}
#endif

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
    case 'M': {
        unsigned long wait_us = strtoul(line + 1, NULL, 10);
        if (wait_us > 1000) {
            send_line("E bad M command\n");
            return;
        }
        cmd.wait_us = wait_us;
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
        } else if (cmd.type == 'M') {
            matrix_probe(cmd.wait_us);
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
