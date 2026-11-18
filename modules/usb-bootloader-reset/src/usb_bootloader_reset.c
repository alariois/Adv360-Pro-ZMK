/*
 * Reboot into the UF2 bootloader when the host "touches" a CDC ACM port
 * with a magic baud rate, e.g. `stty -F /dev/ttyACM0 1200`.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zephyr_cdc_acm_uart

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_bootloader_reset, CONFIG_ZMK_LOG_LEVEL);

/* Same value as RST_UF2 in dt-bindings/zmk/reset.h, used by &bootloader. */
#define UF2_BOOTLOADER_MAGIC 0x57

/*
 * Reboot from a kernel timer (ISR context) rather than the system workqueue:
 * the workqueue can be busy for a long time (e.g. split/BLE work while the
 * peripheral half is unreachable), and the reset must happen regardless.
 */
static void reboot_to_bootloader(struct k_timer *timer) {
    ARG_UNUSED(timer);
    sys_reboot(UF2_BOOTLOADER_MAGIC);
}

static K_TIMER_DEFINE(reboot_timer, reboot_to_bootloader, NULL);

static void dte_rate_changed(const struct device *dev, uint32_t rate) {
    ARG_UNUSED(dev);
    if (rate == CONFIG_ZMK_USB_BOOTLOADER_RESET_BAUD) {
        LOG_INF("Magic baud rate %u, rebooting into UF2 bootloader", rate);
        k_timer_start(&reboot_timer, K_MSEC(CONFIG_ZMK_USB_BOOTLOADER_RESET_DELAY_MS), K_NO_WAIT);
    }
}

#define REGISTER_CDC_ACM(n)                                                                        \
    do {                                                                                           \
        const struct device *dev = DEVICE_DT_GET(DT_DRV_INST(n));                                  \
        if (device_is_ready(dev)) {                                                                \
            int err = cdc_acm_dte_rate_callback_set(dev, dte_rate_changed);                        \
            if (err) {                                                                             \
                LOG_ERR("Failed to register DTE rate callback on %s: %d", dev->name, err);         \
            }                                                                                      \
        }                                                                                          \
    } while (0);

static int usb_bootloader_reset_init(void) {
    DT_INST_FOREACH_STATUS_OKAY(REGISTER_CDC_ACM)
    return 0;
}

SYS_INIT(usb_bootloader_reset_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
