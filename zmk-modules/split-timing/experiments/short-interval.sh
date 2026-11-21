#!/bin/sh
# EXPERIMENT (applied inside the build container, see bin/build.sh
# ZEPHYR_PATCH): let the controller create connections with intervals below
# the Bluetooth minimum of 7.5 ms (6 units). The host check is skipped with
# CONFIG_BT_CONN_PARAM_ANY=y. Both halves run Zephyr's controller, whose
# link layer accepts any interval >= 1 (1.25 ms).
set -e
f=/app/zephyr/subsys/bluetooth/controller/hci/hci.c
sed -i 's/conn_interval_max < 0x0006 ||/conn_interval_max < 0x0001 ||/' "$f"
grep -q "conn_interval_max < 0x0001" "$f"
echo "short-interval patch applied"
