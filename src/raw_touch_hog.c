/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from ZMK's app/src/hog.c (MIT), which in turn derives from
 * Zephyr's samples/bluetooth/peripheral_hids (Apache-2.0, Copyright (c)
 * 2018 Nordic Semiconductor ASA).
 *
 * A second HID-over-GATT service instance carrying only the raw touch
 * report.
 *
 * ZMK core defines its own BT_GATT_SERVICE_DEFINE(..., BT_UUID_HIDS, ...);
 * this is an additional, independent one. HOGP allows a peripheral to expose
 * more than one HID Service, and a host that walks the GATT database will
 * find both. Nothing in ZMK's hog.c is patched.
 *
 * Host support for two HIDS instances on one peripheral is not universal --
 * see the note on CONFIG_ZMK_RAW_TOUCH_BLE in the module Kconfig.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_raw_touch, CONFIG_ZMK_RAW_TOUCH_LOG_LEVEL);

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

#include <zmk/endpoints_types.h>

#include <zmk/raw_touch/lease.h>
#include <zmk/raw_touch/hid.h>
#include <zmk/raw_touch/transport.h>

#include "raw_touch_txq.h"

enum {
    HIDS_REMOTE_WAKE = BIT(0),
    HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

enum {
    HIDS_INPUT = 0x01,
    HIDS_FEATURE = 0x03,
};

struct hids_info {
    uint16_t version; /* version number of base USB HID Specification */
    uint8_t code;     /* country HID Device hardware is localized for */
    uint8_t flags;
} __packed;

struct hids_report {
    uint8_t id;   /* report id */
    uint8_t type; /* report type */
} __packed;

static struct hids_info info = {
    .version = 0x0000,
    .code = 0x00,
    .flags = HIDS_NORMALLY_CONNECTABLE | HIDS_REMOTE_WAKE,
};

/* Over BLE the report ID is NOT part of the notification payload -- it lives
 * here, in the report reference descriptor. That is why the notify below
 * sends sizeof(body) starting at body, while USB sends the whole struct
 * starting at the report_id byte. */
static struct hids_report touch_input = {
    .id = ZMK_RAW_TOUCH_REPORT_ID,
    .type = HIDS_INPUT,
};

static struct hids_report touch_feature = {
    .id = ZMK_RAW_TOUCH_REPORT_ID,
    .type = HIDS_FEATURE,
};

static uint8_t ctrl_point;

static ssize_t read_hids_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_info));
}

static ssize_t read_hids_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_report));
}

static ssize_t read_hids_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, zmk_raw_touch_report_desc,
                             zmk_raw_touch_report_desc_size);
}

/* A real read callback rather than NULL: macOS and iOS commonly read every
 * report characteristic when they connect, and a NULL read handler answers
 * those with an ATT error. It also gives the boot-time scan below a distinct
 * function pointer to find this characteristic by. */
static ssize_t read_hids_touch_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            void *buf, uint16_t len, uint16_t offset) {
    struct zmk_raw_touch_report_body *body = &zmk_raw_touch_hid_get_last_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, body,
                             sizeof(struct zmk_raw_touch_report_body));
}

static ssize_t read_hids_touch_feature_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                              void *buf, uint16_t len, uint16_t offset) {
    struct zmk_raw_touch_feature_body *body = &zmk_raw_touch_hid_get_feature_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, body,
                             sizeof(struct zmk_raw_touch_feature_body));
}

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("Raw touch notifications %s",
            (value == BT_GATT_CCC_NOTIFY) ? "subscribed" : "unsubscribed");
}

/* Host commands - the lease and tap confirm: the feature report's write
 * path (HOGP report characteristics of type Feature are read/write per
 * HIDS 1.0 §2.5.2). The GATT write carries the 4-byte body alone - the
 * report ID lives in the report reference descriptor, as on the input
 * report's notify path - optionally zero-padded to the feature report's
 * declared length, as Windows writes it. The whole command must arrive in
 * one write: a nonzero offset (a long write) is refused below, so a
 * padded body has to fit the link's ATT MTU.
 *
 * The connection identifies the sender: its peer address maps to the ZMK
 * BLE profile, and the command is scoped to that profile's endpoint
 * instance. Writes from a peer that is not a bonded host profile (e.g. a
 * split peripheral's connection) are refused. */
static ssize_t write_hids_touch_feature_report(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr, const void *buf,
                                               uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));

    if (profile < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }

    struct zmk_endpoint_instance source = {
        .transport = ZMK_TRANSPORT_BLE,
        .ble = {.profile_index = profile},
    };

    int err = zmk_raw_touch_host_command(source, buf, len);

    switch (err) {
    case 0:
        return len;
    case -EMSGSIZE:
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    uint8_t *value = attr->user_data;

    if (offset + len > sizeof(ctrl_point)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(value + offset, buf, len);

    return len;
}

/* Raw touch HID Service Declaration */
BT_GATT_SERVICE_DEFINE(
    raw_touch_hog_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_hids_info,
                           NULL, &info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
                           read_hids_report_map, NULL, NULL),

    /* Input report: notified per touch frame, and readable. */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_touch_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &touch_input),

    /* Feature report: pad capabilities on read, host commands on write;
     * no CCC (feature reports are never notified). This is the BLE
     * counterpart of the USB GET_REPORT/SET_REPORT(FEATURE) paths; hosts
     * must read and validate it before treating the collection as raw
     * touch. Feature reports are read/write per HIDS 1.0 s2.5.2; the
     * write permission is a characteristic property only and does not
     * change the report map or attribute layout, so it does not
     * invalidate a host's cached GATT database. */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_hids_touch_feature_report, write_hids_touch_feature_report, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &touch_feature),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl_point, &ctrl_point));

/* Index of the input report's *characteristic declaration* attribute, which
 * is what bt_gatt_notify_cb() wants (it walks forward to the value handle
 * itself). Discovered at boot rather than hand-counted, because the
 * characteristics above can become Kconfig-conditional and a literal index
 * would then silently point at the wrong attribute. 0 means "not found":
 * attrs[0] is the primary service declaration, never a report. */
static size_t touch_input_attr_idx;

/* The module's own work queue for notifications, so that a frame burst can
 * never delay ZMK's own HID reports. Sized and prioritized like ZMK's BLE
 * work queue, which does the same job for those reports. */
K_THREAD_STACK_DEFINE(raw_touch_hog_q_stack, CONFIG_ZMK_BLE_THREAD_STACK_SIZE);

static struct k_work_q raw_touch_hog_work_q;

/* ---------------------------------------------------------------------
 * Send queue
 *
 * Frames wait here for a connection event. The ring itself - the
 * never-evict-a-release policy and the FIFO ordering - lives in
 * raw_touch_txq.h and is shared with the USB transport, so the eviction
 * policy is written once. What is BLE-specific is here:
 *
 *  1. Retried on a transient notify error. -ENOMEM/-ENOBUFS from
 *     bt_gatt_notify_cb() just means no TX buffer is free this connection
 *     event; the frame is fine and the next event will take it. A release
 *     frame therefore stays in the drain's hand and the drain re-arms
 *     itself, up to BLE_RELEASE_ATTEMPTS times. Motion frames are still
 *     dropped on error - retrying them would add latency to data that is
 *     already stale. The retry is head-of-line: nothing behind a stuck
 *     release is sent past it, so a later touch's motion can never
 *     overtake the release that closed the previous one.
 *  2. Bound to a BLE profile. Each entry carries the profile index that
 *     was active when its frame was SAMPLED, and is delivered only while
 *     that profile is the active one. Without that, a frame sampled for
 *     host A - a release waiting on its delayed retry, most damagingly -
 *     is delivered to host B after a profile switch, carrying A's
 *     lease_held bit and a position from A's gesture, which is exactly
 *     the endpoint scoping the protocol promises. So a switch to another
 *     profile discards every queued entry bound elsewhere at once, whether
 *     or not the new profile is connected (a later switch back must not
 *     revive them), and the disconnect of a bound profile discards that
 *     profile's entries, so none of them reaches its host when it
 *     reconnects. Switching ZMK's output between USB and BLE discards
 *     nothing, as on USB: a frame bound to the active profile still
 *     belongs to that host.
 * ------------------------------------------------------------------ */

/* Bounded retry for a release frame stuck behind a transient notify
 * error. BLE_RELEASE_ATTEMPTS - 1 re-arms of the drain, one BLE
 * connection interval apart, so the worst case (~24 ms) stays far inside
 * the host's ~150 ms silence watchdog. */
#define BLE_RELEASE_ATTEMPTS 4
#define BLE_RELEASE_RETRY_DELAY K_MSEC(8)

RT_TXQ_DEFINE(ble_txq, CONFIG_ZMK_RAW_TOUCH_BLE_QUEUE_SIZE);

static void raw_touch_hog_drain(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(raw_touch_hog_work, raw_touch_hog_drain);

/* The frame the drain has taken out of the ring and not finished with:
 * only ever a release waiting for its retry, since every other outcome
 * finishes the frame within the same run. Touched only by the drain
 * handler, which a work queue never runs concurrently with itself, so
 * nothing else can evict, discard or reorder it; the drain itself
 * re-checks it against the active profile and connection on every run. */
static struct rt_txq_entry in_hand;
static bool have_in_hand;
/* Notify attempts already spent on in_hand. */
static uint8_t in_hand_attempts;

static void raw_touch_hog_drop_in_hand(const char *why) {
    LOG_DBG("Discarding raw touch frame for pad %u queued for BLE profile %d (%s)",
            in_hand.body.pad_id, in_hand.binding, why);
    have_in_hand = false;
}

static void raw_touch_hog_drain(struct k_work *work) {
    ARG_UNUSED(work);

    /* One connection lookup per drain, not per frame: the queue holds
     * several frames exactly when BLE batching is in play. The reference
     * zmk_ble_active_profile_conn() hands back is released at the end.
     *
     * The profile is read before the lookup and again after it: while it
     * still reads the same, `conn` is its connection. */
    int conn_profile = zmk_ble_active_profile_index();
    struct bt_conn *conn = zmk_ble_active_profile_conn();

    if (conn == NULL) {
        /* The active profile's host is not connected. Its queued frames
         * were discarded when it disconnected (raw_touch_hog_disconnected())
         * and no more are queued while it is away
         * (zmk_raw_touch_hog_send_report()); anything bound elsewhere was
         * discarded by the profile switch. Nothing queued can be waiting
         * for this profile, so the queue simply waits for the next frame.
         * A release in hand belongs to a connection that is gone. */
        if (have_in_hand) {
            raw_touch_hog_drop_in_hand("its host is not connected");
        }
        return;
    }

    for (;;) {
        int active_profile = zmk_ble_active_profile_index();

        if (active_profile != conn_profile) {
            /* The profile switched since `conn` was looked up: start over
             * with the new profile's connection. */
            k_work_schedule_for_queue(&raw_touch_hog_work_q, &raw_touch_hog_work, K_NO_WAIT);
            break;
        }

        if (!have_in_hand) {
            if (!rt_txq_pop(&ble_txq, &in_hand)) {
                break;
            }
            have_in_hand = true;
            in_hand_attempts = 0;
        }

        /* The delivery rule: a frame goes out while the profile it was
         * sampled for is the active BLE profile, whatever ZMK's output is
         * now. The profile switch discards the queued ones bound
         * elsewhere; this catches the one in hand, and a switch that
         * landed between a pop and this check. Its lease_held bit and its
         * coordinates describe its own host's gesture, which that host's
         * silence watchdog closes. */
        if (in_hand.binding != active_profile) {
            raw_touch_hog_drop_in_hand("another profile is active");
            continue;
        }

        struct bt_gatt_notify_params notify_params = {
            .attr = &raw_touch_hog_svc.attrs[touch_input_attr_idx],
            .data = &in_hand.body,
            .len = sizeof(in_hand.body),
        };

        int err = bt_gatt_notify_cb(conn, &notify_params);

        if (err == -EPERM) {
            bt_conn_set_security(conn, BT_SECURITY_L2);
        }

        if (err == 0) {
            have_in_hand = false;
            continue;
        }

        /* -EINVAL is a host that has not subscribed to the report
         * (CONFIG_BT_GATT_ENFORCE_SUBSCRIPTION), which no retry changes. */
        if (rt_txq_is_release(&in_hand.body) && err != -EINVAL &&
            in_hand_attempts + 1 < BLE_RELEASE_ATTEMPTS) {
            /* Keep it in hand and come back: no TX buffer now does not
             * mean none next connection event. Deliberately head-of-line -
             * sending the frames behind it first would put a later touch's
             * motion ahead of this touch's release. */
            in_hand_attempts++;
            k_work_reschedule_for_queue(&raw_touch_hog_work_q, &raw_touch_hog_work,
                                        BLE_RELEASE_RETRY_DELAY);
            break;
        }

        if (rt_txq_is_release(&in_hand.body)) {
            /* Not a passing buffer shortage: nothing to do but let the
             * host's silence watchdog close the gesture. */
            LOG_WRN("Dropped raw touch release frame for pad %u after %d notify failures (%d)",
                    in_hand.body.pad_id, in_hand_attempts + 1, err);
        } else {
            LOG_DBG("Error notifying %d", err);
        }

        have_in_hand = false;
    }

    bt_conn_unref(conn);
}

/* A host's connection is gone: its lease lapses, and the frames queued
 * for it can no longer be delivered.
 *
 * Our own connection callback rather than zmk_ble_active_profile_changed:
 * that event fires only for the ACTIVE profile, while a lease and a queued
 * frame can belong to any connected profile. zmk_ble_profile_index() maps
 * the peer to its profile, returning a negative value for non-host
 * connections such as a split peripheral's, which hold no lease and no
 * frames. A release the drain holds for this profile is dropped by the
 * drain itself on its retry, which is already armed. */
static void raw_touch_hog_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));

    if (profile < 0) {
        return;
    }

    struct zmk_endpoint_instance endpoint = {
        .transport = ZMK_TRANSPORT_BLE,
        .ble = {.profile_index = profile},
    };

    zmk_raw_touch_lease_clear(endpoint, "BLE profile disconnected");
    rt_txq_discard_binding(&ble_txq, (int16_t)profile);
}

BT_CONN_CB_DEFINE(zmk_raw_touch_hog_conn_callbacks) = {
    .disconnected = raw_touch_hog_disconnected,
};

/* Another profile became active (or the active one connected or
 * disconnected, which the same event reports): the queued frames bound to
 * any other profile can no longer be delivered. A release the drain holds
 * for another profile is dropped by the drain itself on its retry, which
 * is already armed. */
static int raw_touch_hog_profile_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);

    if (ev != NULL) {
        rt_txq_keep_only_binding(&ble_txq, (int16_t)ev->index);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_raw_touch_hog, raw_touch_hog_profile_listener);
ZMK_SUBSCRIPTION(zmk_raw_touch_hog, zmk_ble_active_profile_changed);

int zmk_raw_touch_hog_send_report(int profile, const struct zmk_raw_touch_report_body *body) {
    if (touch_input_attr_idx == 0) {
        return -ENODEV;
    }

    /* The entry is bound to @p profile, the profile the frame was sampled
     * for, rather than to whatever is active when it is drained. */
    /* A frame for a host that is not connected could only reach it after
     * it reconnects, stale - the trailing release that a disconnect in
     * mid-touch produces, typically. The disconnect path discards what was
     * queued before it; this keeps anything from queueing after it. */
    if (!zmk_ble_profile_is_connected((uint8_t)profile)) {
        return -ENOTCONN;
    }

    /* Never blocks: this runs inline on the input dispatch path, where
     * waiting would head-of-line block pointer deltas and taps. */
    int err = rt_txq_enqueue(&ble_txq, body, (int16_t)profile, "BLE");

    /* K_NO_WAIT via k_work_schedule (not reschedule): if a release-frame
     * retry is already armed, leave its delay alone - the queue cannot be
     * drained past that frame anyway. Zephyr's k_work_schedule submits
     * when the item is idle *or running*, so a frame arriving mid-drain
     * still gets a wakeup. */
    k_work_schedule_for_queue(&raw_touch_hog_work_q, &raw_touch_hog_work, K_NO_WAIT);

    return err;
}

static int raw_touch_hog_init(void) {
    /* BT_GATT_CHARACTERISTIC() emits two attributes -- the declaration and
     * then the value -- so the declaration sits one before the attribute
     * carrying our read callback. */
    for (size_t i = 0; i < raw_touch_hog_svc.attr_count; i++) {
        if (raw_touch_hog_svc.attrs[i].read == read_hids_touch_input_report) {
            touch_input_attr_idx = i - 1;
            break;
        }
    }

    if (touch_input_attr_idx == 0) {
        LOG_ERR("Could not locate the raw touch input report characteristic");
        return -ENODEV;
    }

    static const struct k_work_queue_config queue_config = {.name = "Raw Touch HOG Send Work"};
    k_work_queue_start(&raw_touch_hog_work_q, raw_touch_hog_q_stack,
                       K_THREAD_STACK_SIZEOF(raw_touch_hog_q_stack), CONFIG_ZMK_BLE_THREAD_PRIORITY,
                       &queue_config);

    return 0;
}

SYS_INIT(raw_touch_hog_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
