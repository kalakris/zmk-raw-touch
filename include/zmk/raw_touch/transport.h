/*
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/endpoints_types.h>

#include <zmk/raw_touch/hid.h>

/* Both transports queue frames in the ring of src/raw_touch_txq.h, which
 * is where the guarantees for release frames - never evicted, delivered
 * in order - are stated. Only what is transport-specific is noted below. */

/* An endpoint a frame is sent to: the instance, and ZMK's
 * zmk_endpoint_instance_to_index() of it, so equal indices mean the same
 * host (transport and BLE profile). */
struct zmk_raw_touch_endpoint {
    struct zmk_endpoint_instance instance;
    /* -1 on a build with no host-facing transport, which never sends. */
    int index;
};

/**
 * @brief The endpoint ZMK sends to now, i.e. zmk_endpoints_selected().
 *
 * The frame handler reads it once per frame and passes it to everything
 * that frame does, so the lease check, the frame's flags and its
 * destination cannot straddle an endpoint switch. Defined in
 * src/raw_touch_endpoints.c.
 */
struct zmk_raw_touch_endpoint zmk_raw_touch_selected_endpoint(void);

/**
 * @brief Send a raw touch frame to @p ep.
 *
 * Dispatches on the endpoint's transport exactly as ZMK's own reports do,
 * so the raw frames follow the user's USB/BLE output selection and their
 * active BLE profile. Both transports copy @p body into their queue before
 * returning. Defined in src/raw_touch_endpoints.c.
 *
 * @retval 0 on success, or a negative errno.
 */
int zmk_raw_touch_send_report(struct zmk_raw_touch_endpoint ep,
                              const struct zmk_raw_touch_report_body *body);

/* The two transports' send functions are declared unconditionally so that
 * src/raw_touch_endpoints.c can dispatch with IS_ENABLED(); each is defined
 * only when its transport is built, and the call to a disabled one is
 * compiled out. */

/**
 * @brief Queue a frame for the raw touch USB HID interface.
 *
 * Never blocks - it runs inline on the input dispatch path. The frame is
 * copied into the queue; if the interrupt IN endpoint is idle it is
 * written straight away, and otherwise the completion of the transfer
 * already in flight takes it, so a busy endpoint delays a frame rather
 * than losing it.
 *
 * While the bus is suspended the frame is dropped and a remote wakeup is
 * requested, and a suspend flushes anything still queued: frames held
 * until the host resumes would describe a touch from before it slept.
 *
 * A bus reset or detach flushes the queue: those frames belong to a bus
 * that is gone, and the host's silence watchdog is what closes that
 * gesture. Defined in src/raw_touch_usb_hid.c.
 *
 * @retval 0 if the frame was queued, or queued and written.
 * @retval -ENODEV if the interface is missing or the bus is not in the
 *         HID state.
 * @retval -EAGAIN if the bus is suspended (the frame is dropped).
 * @retval -ENOBUFS if the queue was full of undelivered release frames
 *         (a mis-sized CONFIG_ZMK_RAW_TOUCH_USB_QUEUE_SIZE).
 * @retval other negative errno from hid_int_ep_write().
 */
int zmk_raw_touch_usb_send_report(const struct zmk_raw_touch_report_body *body);

/**
 * @brief Queue a frame for notification on the raw touch HIDS instance,
 *        for the host of BLE profile @p profile.
 *
 * Never blocks - it runs inline on the input dispatch path. The frame is
 * copied into the queue and a work queue notifies it. A release frame that
 * fails to notify (typically -ENOMEM/-ENOBUFS: no TX buffer free this
 * connection event) is retried a few times, one connection interval
 * apart, and the retry is head-of-line, so nothing overtakes it.
 *
 * Each entry is bound to the BLE profile that was active when its frame
 * was sampled. A frame is delivered only to that profile: after a profile
 * switch or the disconnect of the bound profile, queued frames - a
 * retrying release included - are discarded rather than sent to whoever
 * is connected now, so the protocol's endpoint scoping holds for queued
 * frames too.
 *
 * Motion frames are still dropped under pressure: they are ~10 ms apart
 * and carry absolute positions, so a gap costs nothing but the `seq`
 * counter noting it. Defined in src/raw_touch_hog.c.
 *
 * @retval 0 if the frame was queued.
 * @retval -ENODEV if the input report characteristic was not found.
 * @retval -ENOTCONN if the active profile's host is not connected.
 * @retval -ENOBUFS if the queue was full of undelivered release frames
 *         (a mis-sized CONFIG_ZMK_RAW_TOUCH_BLE_QUEUE_SIZE).
 */
int zmk_raw_touch_hog_send_report(int profile, const struct zmk_raw_touch_report_body *body);
