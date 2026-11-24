/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from ZMK's app/src/endpoints.c (MIT).
 *
 * Endpoint dispatch for the raw touch report.
 *
 * Mirrors ZMK core's zmk_endpoints_send_*_report(): the raw frames follow
 * whichever transport the user has selected, and on BLE whichever profile is
 * active, exactly as the keyboard and mouse reports do.
 *
 * With no host-facing transport built (both disabled in the .conf, or a
 * zmk,raw-touch-pad node on a split peripheral, where the transports'
 * central-only dependency turns both off) the frame handler still links
 * against this file and still provides Standard mode - the relative deltas
 * and taps it re-injects need no transport - so only the raw frames are
 * unavailable and the send is a no-op. ZMK's endpoints API does not exist
 * on a peripheral (ZMK builds src/endpoints.c only on a central), so every
 * call into it sits behind IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_HOST_TRANSPORT)
 * and is compiled out there.
 */

#include <errno.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_raw_touch, CONFIG_ZMK_RAW_TOUCH_LOG_LEVEL);

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>

#include <zmk/raw_touch/hid.h>
#include <zmk/raw_touch/transport.h>

struct zmk_raw_touch_endpoint zmk_raw_touch_selected_endpoint(void) {
    struct zmk_raw_touch_endpoint ep = {.index = -1};

    if (IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_HOST_TRANSPORT)) {
        ep.instance = zmk_endpoints_selected();
        ep.index = zmk_endpoint_instance_to_index(ep.instance);
    }

    return ep;
}

int zmk_raw_touch_send_report(struct zmk_raw_touch_endpoint ep,
                              const struct zmk_raw_touch_report_body *body) {
    if (!IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_HOST_TRANSPORT)) {
        return -ENOTSUP;
    }

    int err;

    /* Failures are logged at debug level only: frames stream at ~100 Hz
     * while a finger is down, so a host that has gone away would otherwise
     * flood the log at the default level. */
    switch (ep.instance.transport) {
    case ZMK_TRANSPORT_USB:
        if (!IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_USB)) {
            LOG_DBG("Raw touch over USB is not enabled");
            return -ENOTSUP;
        }

        err = zmk_raw_touch_usb_send_report(body);
        if (err) {
            LOG_DBG("Failed to send raw touch report over USB: %d", err);
        }
        return err;

    case ZMK_TRANSPORT_BLE:
        if (!IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_BLE)) {
            LOG_DBG("Raw touch over BLE is not enabled");
            return -ENOTSUP;
        }

        err = zmk_raw_touch_hog_send_report(ep.instance.ble.profile_index, body);
        if (err) {
            LOG_DBG("Failed to send raw touch report over HOG: %d", err);
        }
        return err;
    }

    LOG_ERR("Unhandled endpoint transport %d", ep.instance.transport);
    return -ENOTSUP;
}
