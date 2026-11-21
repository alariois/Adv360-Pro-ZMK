/*
 * Hardware watchdog: a hung half resets itself (see Kconfig).
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(zmk_watchdog, CONFIG_ZMK_LOG_LEVEL);

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int channel;
static uint32_t reset_reason;

/* RESETREAS at boot: bit 1 = watchdog, 0 = reset pin, 2 = soft reset ... */
uint32_t zmk_reset_reason(void) { return reset_reason; }

/* Breadcrumbs: a RAM ring that survives the watchdog reset (not zeroed at
 * boot). Code that might hang calls zmk_crumb(id); after a reset the key
 * monitor's C command shows the previous boot's last crumbs. */
#define CRUMBS 32
struct crumbs {
    uint32_t magic;
    uint32_t next;
    uint32_t entry[CRUMBS]; /* id << 24 | uptime ms & 0xffffff */
};
static __noinit struct crumbs crumbs;
static struct crumbs prev_crumbs;

void zmk_crumb(uint8_t id) {
    unsigned int key = irq_lock();
    crumbs.entry[crumbs.next % CRUMBS] = ((uint32_t)id << 24) | (k_uptime_get_32() & 0xffffff);
    crumbs.next++;
    irq_unlock(key);
}

/* previous boot's crumbs, oldest first; returns how many */
int zmk_prev_crumbs(uint32_t *out, int max) {
    int n = MIN(MIN(prev_crumbs.next, CRUMBS), max);
    for (int i = 0; i < n; i++) {
        out[i] = prev_crumbs.entry[(prev_crumbs.next - n + i) % CRUMBS];
    }
    return n;
}

static void feed(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(feed_work, feed);

static void feed(struct k_work *work) {
    zmk_crumb(40);
    wdt_feed(wdt, channel);
    k_work_reschedule(&feed_work, K_MSEC(CONFIG_ZMK_WATCHDOG_TIMEOUT_MS / 4));
}

static int zmk_watchdog_init(void) {
    if (crumbs.magic == 0xc4b5c4b5) {
        prev_crumbs = crumbs;
    }
    crumbs.magic = 0xc4b5c4b5;
    crumbs.next = 0;
    reset_reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xffffffff; /* bits are sticky: clear for next time */

    if (!device_is_ready(wdt)) {
        LOG_ERR("watchdog not ready");
        return -ENODEV;
    }
    struct wdt_timeout_cfg cfg = {
        .window = {.min = 0, .max = CONFIG_ZMK_WATCHDOG_TIMEOUT_MS},
        .flags = WDT_FLAG_RESET_SOC,
    };
    channel = wdt_install_timeout(wdt, &cfg);
    if (channel < 0) {
        LOG_ERR("watchdog timeout: %d", channel);
        return channel;
    }
    /* keeps counting while the CPU sleeps, not while a debugger halts it */
    int err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (err) {
        LOG_ERR("watchdog setup: %d", err);
        return err;
    }
    k_work_reschedule(&feed_work, K_MSEC(CONFIG_ZMK_WATCHDOG_TIMEOUT_MS / 4));
    return 0;
}

SYS_INIT(zmk_watchdog_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
