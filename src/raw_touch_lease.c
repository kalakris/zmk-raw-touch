/*
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * Host-lease state: per-endpoint-instance leases with timeout expiry.
 *
 * One lease slot per selectable endpoint instance, indexed by ZMK core's
 * own zmk_endpoint_instance_to_index(). A slot, not a single global
 * lease, because a global boolean is a latent multi-host bug: host A
 * leasing over USB must never mute (or steal) the fallback that host B
 * on a BLE profile relies on. Each endpoint's host leases independently;
 * only the lease belonging to the endpoint ZMK is currently sending to
 * ever suppresses anything.
 *
 * Liveness: a slot holds one thing, the uptime at which its lease
 * expires, and a lease is live exactly while that is in the future. Every
 * acquire/renew write moves it; nothing has to run at the deadline for
 * the lease to lapse, because every reader compares against the clock.
 * In particular the frame handler asks once per frame, so frame
 * emission, flags bit 2 and the wheel suppression all revert on the first
 * frame after the deadline, and the trailing release for a touch in
 * progress goes out on that frame too. A slot found past its deadline is
 * reset and its expiry logged then, by whichever reader finds it first.
 *
 * Leases also lapse eagerly on the events that make the leasing host
 * unreachable or irrelevant:
 *
 *  - USB detach/reset (zmk_usb_conn_state_changed leaving the HID state)
 *    clears the USB lease;
 *  - disconnect of a BLE connection clears the lease of the profile
 *    bonded to that peer, whether or not it is the active profile (the
 *    BLE transport reports it through zmk_raw_touch_lease_clear());
 *  - an endpoint switch clears every lease except the newly selected
 *    endpoint's own. The leasing host may no longer be watching after a
 *    switch, and a wrongly-cleared lease self-heals: a live host
 *    renews at most timeout/2 later, re-acquiring the lease.
 *
 * Threading: lease writes arrive on the USB workqueue or the BT RX
 * thread, the lapse events on the system workqueue and the BT RX thread,
 * and the liveness check on the input dispatch path. Every access to a
 * slot is one critical section under lease_lock, which also keeps the
 * 64-bit deadline from being read torn on a 32-bit core.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_raw_touch, CONFIG_ZMK_RAW_TOUCH_LOG_LEVEL);

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>

#include <zmk/raw_touch/lease.h>
#include <zmk/raw_touch/tap.h>

#if IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

/* k_uptime_get() at which each endpoint's lease lapses; 0 = no lease.
 * Uptime is 64-bit milliseconds and never wraps, and a live deadline is
 * always later than the boot-time 0. */
static int64_t lease_expires_at[ZMK_ENDPOINT_COUNT];
static struct k_spinlock lease_lock;

static bool lease_index_valid(int idx) { return idx >= 0 && idx < ZMK_ENDPOINT_COUNT; }

/* Whether the slot holds a live lease at @p now. A slot past its deadline
 * is reset, and *expired is set so the caller logs the lapse once it has
 * left the critical section. Caller holds lease_lock. */
static bool lease_live_locked(int idx, int64_t now, bool *expired) {
    if (lease_expires_at[idx] != 0 && now >= lease_expires_at[idx]) {
        lease_expires_at[idx] = 0;
        *expired = true;
    }

    return lease_expires_at[idx] != 0;
}

static void lease_log_expired(int idx) {
    LOG_WRN("Raw touch host lease for endpoint %d expired without a renewal; "
            "wheel fallback restored",
            idx);
}

static void lease_clear_index(int idx, const char *reason) {
    if (!lease_index_valid(idx)) {
        return;
    }

    bool expired = false;
    bool cleared = false;

    K_SPINLOCK(&lease_lock) {
        cleared = lease_live_locked(idx, k_uptime_get(), &expired);
        lease_expires_at[idx] = 0;
    }

    if (expired) {
        lease_log_expired(idx);
    }
    if (cleared) {
        LOG_INF("Raw touch host lease for endpoint %d cleared (%s)", idx, reason);
    }
}

/* A command is its ZMK_RAW_TOUCH_CMD_LEN-byte body, optionally followed by
 * zero padding: Windows' HidD_SetFeature, and hidapi on Windows, always
 * write the feature report's full declared length. Anything shorter, or
 * a nonzero byte in the padding, is malformed. */
static bool command_length_valid(const uint8_t *body, size_t len) {
    if (len < ZMK_RAW_TOUCH_CMD_LEN) {
        return false;
    }

    for (size_t i = ZMK_RAW_TOUCH_CMD_LEN; i < len; i++) {
        if (body[i] != 0) {
            return false;
        }
    }

    return true;
}

int zmk_raw_touch_host_command(struct zmk_endpoint_instance source, const uint8_t *body,
                               size_t len) {
    if (!command_length_valid(body, len)) {
        LOG_WRN("Rejected host command with length %d or nonzero padding", (int)len);
        return -EMSGSIZE;
    }

    if (body[0] == ZMK_RAW_TOUCH_CMD_TAP_CONFIRM) {
        if (body[2] != 0 || body[3] != 0) {
            LOG_WRN("Rejected tap confirm with nonzero reserved bytes");
            return -EINVAL;
        }

        /* Only the leasing, selected endpoint has seen the touch it is
         * confirming; a stale confirmation from another endpoint (after a
         * switch, or a lapsed lease) must not click. Not an error. */
        int source_idx = zmk_endpoint_instance_to_index(source);

        if (source_idx != zmk_endpoint_instance_to_index(zmk_endpoints_selected()) ||
            !zmk_raw_touch_lease_held(source_idx)) {
            LOG_DBG("Ignored tap confirm for pad %d from an endpoint without the lease", body[1]);
            return 0;
        }

        return zmk_raw_touch_tap_confirm(body[1]);
    }

    if (body[0] != ZMK_RAW_TOUCH_CMD_LEASE) {
        LOG_WRN("Rejected unknown lease command 0x%02x", body[0]);
        return -ENOTSUP;
    }

    if (body[3] != 0) {
        LOG_WRN("Rejected lease command with nonzero reserved byte 0x%02x", body[3]);
        return -EINVAL;
    }

    int idx = zmk_endpoint_instance_to_index(source);

    if (!lease_index_valid(idx)) {
        LOG_ERR("Lease command from unindexable endpoint (transport %d)", source.transport);
        return -EINVAL;
    }

    char label[ZMK_ENDPOINT_STR_LEN];
    zmk_endpoint_instance_to_str(source, label, sizeof(label));

    switch (body[1]) {
    case ZMK_RAW_TOUCH_LEASE_OP_ACQUIRE: {
        uint8_t timeout_s = body[2];

        if (timeout_s == 0) {
            LOG_WRN("Rejected lease with zero timeout");
            return -EINVAL;
        }

        timeout_s =
            CLAMP(timeout_s, ZMK_RAW_TOUCH_LEASE_TIMEOUT_MIN_S, ZMK_RAW_TOUCH_LEASE_TIMEOUT_MAX_S);

        bool expired = false;
        bool renewed = false;

        K_SPINLOCK(&lease_lock) {
            int64_t now = k_uptime_get();

            renewed = lease_live_locked(idx, now, &expired);
            lease_expires_at[idx] = now + (int64_t)timeout_s * MSEC_PER_SEC;
        }

        if (expired) {
            lease_log_expired(idx);
        }

        if (!renewed) {
            LOG_INF("Raw touch lease acquired by %s (timeout %us); wheel fallback suppressed "
                    "while %s is selected",
                    label, timeout_s, label);
        } else {
            LOG_DBG("Raw touch lease renewed by %s (timeout %us)", label, timeout_s);
        }

        return 0;
    }

    case ZMK_RAW_TOUCH_LEASE_OP_RELEASE:
        /* body[2] (timeout) is ignored on release. Releasing without a
         * lease is a harmless no-op: release must be safe to send from
         * host shutdown paths. Scoping makes this per-endpoint, so one
         * host's release can never clear another endpoint's lease. */
        lease_clear_index(idx, "released by host");
        return 0;

    default:
        LOG_WRN("Rejected lease command with unknown operation 0x%02x", body[1]);
        return -EINVAL;
    }
}

void zmk_raw_touch_lease_clear(struct zmk_endpoint_instance endpoint, const char *reason) {
    lease_clear_index(zmk_endpoint_instance_to_index(endpoint), reason);
}

bool zmk_raw_touch_lease_held(int endpoint) {
    if (!lease_index_valid(endpoint)) {
        return false;
    }

    bool expired = false;
    bool held = false;

    K_SPINLOCK(&lease_lock) { held = lease_live_locked(endpoint, k_uptime_get(), &expired); }

    if (expired) {
        lease_log_expired(endpoint);
    }

    return held;
}

static int lease_event_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *epc = as_zmk_endpoint_changed(eh);

    if (epc != NULL) {
        /* Endpoint switch: clear every lease except the newly selected
         * endpoint's own, so a host already holding a lease on the
         * switched-to endpoint takes over seamlessly while leases left
         * behind cannot go stale out of sight. */
        int selected = zmk_endpoint_instance_to_index(epc->endpoint);

        for (int i = 0; i < ZMK_ENDPOINT_COUNT; i++) {
            if (i != selected) {
                lease_clear_index(i, "endpoint switched away");
            }
        }

        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_USB)
    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(eh);

    if (usb != NULL && usb->conn_state != ZMK_USB_CONN_HID) {
        struct zmk_endpoint_instance usb_endpoint = {.transport = ZMK_TRANSPORT_USB};

        lease_clear_index(zmk_endpoint_instance_to_index(usb_endpoint), "USB detached");
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_raw_touch_lease, lease_event_listener);
ZMK_SUBSCRIPTION(zmk_raw_touch_lease, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_USB)
ZMK_SUBSCRIPTION(zmk_raw_touch_lease, zmk_usb_conn_state_changed);
#endif
