/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>

#include "bs_tracing.h"
#include "bs_types.h"
#include "bstests.h"
#include "time_machine.h"

#include <zephyr/sys/__assert.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/logging/log.h>

#include <bluetooth/nrf/host_extensions.h>

#include "babblekit/testcase.h"
#include "babblekit/flags.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

DEFINE_FLAG(flag_is_connected);
DEFINE_FLAG(flag_sec_lvl_changed);

static struct bt_conn *test_conn;
static struct bt_nrf_ltk test_ltk = {{0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf},};

static bool authenticated;

static void set_custom_ltk(void)
{
    int err;

    err = bt_nrf_conn_set_ltk(test_conn, &test_ltk, authenticated);
    TEST_ASSERT(!err, "bt_nrf_conn_set_ltk failed (%d).", err);
}

static void clear_conn(void)
{
    if (test_conn) {
        bt_conn_unref(test_conn);
        test_conn = NULL;
    }
}

static void check_sec_info(struct bt_conn *conn)
{
    int err;
    struct bt_conn_info info;

    err = bt_conn_get_info(conn, &info);
    TEST_ASSERT(!err, "bt_conn_get_info failed (%d).", err);

    bt_security_t sec_lvl = authenticated ? BT_SECURITY_L4 : BT_SECURITY_L2;
    TEST_ASSERT(info.security.level == sec_lvl, "Security level mismatch got %u, expected %u",
                info.security.level, sec_lvl);
    TEST_ASSERT(info.security.enc_key_size == sizeof(test_ltk), "Key size didn't match");
    TEST_ASSERT(info.security.flags == BT_SECURITY_FLAG_SC, "Sec flags didn't match");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    TEST_ASSERT((!test_conn || (conn == test_conn)), "Unexpected new connection.");

    if (!test_conn) {
        test_conn = bt_conn_ref(conn);
    }

    if (err != 0) {
        clear_conn();
        TEST_ASSERT(conn, "Connection attempt failed with %d", err);
        return;
    }

    LOG_INF("Connected");
    set_custom_ltk();

    SET_FLAG(flag_is_connected);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    UNSET_FLAG(flag_is_connected);
}

static void sec_changed(struct bt_conn *conn, bt_security_t level,
                enum bt_security_err err)
{
    TEST_ASSERT(!err, "Security level update failed (%d).", err);

    LOG_INF("Sec level set %u", level);
    check_sec_info(conn);
    flag_sec_lvl_changed = true;
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = sec_changed,
};

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t type, struct net_buf_simple *ad)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    int err;

    if (test_conn != NULL) {
        return;
    }

    /* We're only interested in connectable events */
    if (type != BT_HCI_ADV_IND && type != BT_HCI_ADV_DIRECT_IND) {
        TEST_FAIL("Unexpected advertisement type.");
    }

    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INF("Connecting to dst %s", addr_str);

    err = bt_le_scan_stop();
    TEST_ASSERT(!err, "Err bt_le_scan_stop %d", err);

    err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &test_conn);
    TEST_ASSERT(!err, "Err bt_conn_le_create %d", err);
}

static void scan_and_connect(void)
{
    int err;

    err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_cb);
    TEST_ASSERT(!err, "Err bt_le_scan_start %d", err);
}

static void disconnect(void)
{
    int err;

    err = bt_conn_disconnect(test_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    TEST_ASSERT(!err, "Err bt_conn_disconnect %d", err);
}

static void advertise_connectable(void)
{
    int err;
    struct bt_le_adv_param param = {};

    param.id = BT_ID_DEFAULT;
    param.interval_min = 0x0020;
    param.interval_max = 0x4000;
    param.options |= BT_LE_ADV_OPT_CONN;

    err = bt_le_adv_start(&param, NULL, 0, NULL, 0);
    TEST_ASSERT(err == 0, "Advertising failed to start (err %d)", err);
}

static void set_security(void)
{
	int err;

	err = bt_conn_set_security(test_conn, BT_SECURITY_L2);
	TEST_ASSERT(!err, "Err bt_conn_set_security %d", err);
}

static void test_setup(void)
{
    int err;

    err = bt_enable(NULL);
    TEST_ASSERT(!err, "bt_enable failed.");
}

void central_test(void)
{
    test_setup();

    for (uint8_t i = 0; i < 3; i++) {
        authenticated = i;

        scan_and_connect();
        WAIT_FOR_FLAG(flag_is_connected);

        set_security();
        TAKE_FLAG(flag_sec_lvl_changed);

        disconnect();
        WAIT_FOR_FLAG_UNSET(flag_is_connected);
        clear_conn();
    }

    TEST_PASS("PASS");
}

void peripheral_test(void)
{
    test_setup();

    for (uint8_t i = 0; i < 3; i++) {
        authenticated = i;

        advertise_connectable();
        WAIT_FOR_FLAG(flag_is_connected);

        TAKE_FLAG(flag_sec_lvl_changed);

        WAIT_FOR_FLAG_UNSET(flag_is_connected);
        clear_conn();
    }

    TEST_PASS("PASS");
}

static const struct bst_test_instance test_to_add[] = {
    {
        .test_id = "central_test",
        .test_main_f = central_test,
    },
    {
        .test_id = "peripheral_test",
        .test_main_f = peripheral_test,
    },
    BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *tests)
{
    return bst_add_tests(tests, test_to_add);
};

bst_test_install_t test_installers[] = {install, NULL};

int main(void)
{
    bst_main();
    return 0;
}
