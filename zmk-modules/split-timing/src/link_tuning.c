/*
 * Split link tuning on the central half (see Kconfig).
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(split_link, CONFIG_ZMK_LOG_LEVEL);

__weak void zmk_crumb(uint8_t id) {}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_LINK_2M_PHY)
static struct bt_conn *phy_conn;

static void phy_update(struct k_work *work) {
    if (phy_conn != NULL) {
        int err = bt_conn_le_phy_update(phy_conn, BT_CONN_LE_PHY_PARAM_2M);
        LOG_INF("2M PHY request: %d", err);
        bt_conn_unref(phy_conn);
        phy_conn = NULL;
    }
}
static K_WORK_DELAYABLE_DEFINE(phy_work, phy_update);

static void on_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;
    if (err || bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL ||
        phy_conn != NULL) {
        return;
    }
    phy_conn = bt_conn_ref(conn);
    k_work_reschedule(&phy_work, K_MSEC(3000));
}

BT_CONN_CB_DEFINE(split_link_conn_cb) = {
    .connected = on_connected,
};
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_LINK_NO_ADVERTISING)
static void stop_adv(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_work, stop_adv);

static void stop_adv(struct k_work *work) {
    bt_le_adv_stop();
    k_work_reschedule(&adv_work, K_MSEC(200));
}

static int no_adv_init(void) {
    k_work_reschedule(&adv_work, K_MSEC(1000));
    return 0;
}
SYS_INIT(no_adv_init, APPLICATION, 99);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_LINK_NO_ADV_ON_USB)
#include <zmk/ble.h>
#include <zmk/endpoints.h>

static void check_output(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(output_work, check_output);

static void check_output(struct k_work *work) {
    static bool stopped;
    zmk_crumb(20);
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB) {
        zmk_crumb(21);
        bt_le_adv_stop(); /* ZMK restarts it on some events: stop again */
        zmk_crumb(22);
        stopped = true;
    } else if (stopped) {
        stopped = false;
        /* the public way to make ZMK advertise again (same name) */
        zmk_crumb(23);
        zmk_ble_set_device_name((char *)bt_get_name());
        zmk_crumb(24);
        LOG_INF("output not USB: advertising again");
    }
    k_work_reschedule(&output_work, K_MSEC(250));
}

static int no_adv_on_usb_init(void) {
    k_work_reschedule(&output_work, K_MSEC(1000));
    return 0;
}
SYS_INIT(no_adv_on_usb_init, APPLICATION, 99);
#endif
