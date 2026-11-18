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

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_REGISTER(key_monitor, CONFIG_ZMK_LOG_LEVEL);

#define KEY_MONITOR_VERSION 1

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
    char buf[40];
    snprintf(buf, sizeof(buf), "L %x %u\n", (unsigned int)zmk_keymap_layer_state(),
             (unsigned int)zmk_keymap_highest_layer_active());
    send_line(buf);
}

static void send_hello(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "H key-monitor %d inject=%d keys=%d\n", KEY_MONITOR_VERSION,
             IS_ENABLED(CONFIG_ZMK_KEY_MONITOR_INJECT), ZMK_KEYMAP_LEN);
    send_line(buf);
    send_layers();
}

static int key_monitor_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        char buf[24];
        snprintf(buf, sizeof(buf), "P %u %u\n", (unsigned int)pos->position, pos->state ? 1 : 0);
        send_line(buf);
    } else if (as_zmk_layer_state_changed(eh)) {
        send_layers();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_monitor, key_monitor_listener);
ZMK_SUBSCRIPTION(key_monitor, zmk_position_state_changed);
ZMK_SUBSCRIPTION(key_monitor, zmk_layer_state_changed);

/* ---- host -> device ---------------------------------------------------- */

struct command {
    char type; /* 'K' inject, '?' hello */
    uint8_t state;
    uint16_t position;
};

K_MSGQ_DEFINE(command_queue, sizeof(struct command), 32, 4);

static void handle_line(char *line) {
    struct command cmd = {.type = line[0]};

    switch (line[0]) {
    case '?':
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
    }
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

static void key_monitor_thread(void) {
    bool was_connected = false;
    struct command cmd;

    while (true) {
        /* Wake up regularly to notice a host connecting (DTR raised). */
        if (k_msgq_get(&command_queue, &cmd, K_MSEC(100)) == 0) {
            if (cmd.type == '?') {
                send_hello();
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

        bool connected = host_connected();
        if (connected && !was_connected) {
            send_hello();
        }
        was_connected = connected;
    }
}

K_THREAD_DEFINE(key_monitor_tid, CONFIG_ZMK_KEY_MONITOR_THREAD_STACK_SIZE, key_monitor_thread, NULL,
                NULL, NULL, CONFIG_ZMK_KEY_MONITOR_THREAD_PRIORITY, 0, 0);

static int key_monitor_init(void) {
    if (!device_is_ready(uart)) {
        LOG_ERR("Key monitor UART not ready");
        return -ENODEV;
    }
    uart_irq_callback_set(uart, uart_callback);
    uart_irq_rx_enable(uart);
    LOG_WRN("Key monitor enabled: key presses are readable%s over USB serial",
            IS_ENABLED(CONFIG_ZMK_KEY_MONITOR_INJECT) ? " and injectable" : "");
    return 0;
}

SYS_INIT(key_monitor_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
