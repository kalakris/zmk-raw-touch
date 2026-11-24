/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from ZMK's app/src/usb_hid.c (MIT).
 *
 * A second USB HID interface (HID_1) carrying only the raw touch report.
 *
 * ZMK core owns HID_0. We register HID_1 with our own report descriptor and
 * our own hid_ops, which is why the raw frames need no changes to ZMK's
 * descriptor or endpoint. Both interfaces sit behind the same USB device, so
 * the host sees one composite device exposing two HID collections.
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_raw_touch, CONFIG_ZMK_RAW_TOUCH_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <zmk/raw_touch/lease.h>
#include <zmk/raw_touch/hid.h>
#include <zmk/raw_touch/transport.h>

#include "raw_touch_txq.h"

/* Zephyr instantiates exactly CONFIG_USB_HID_DEVICE_COUNT HID devices, named
 * HID_0 .. HID_(N-1). With the default of 1 there is no HID_1, and the only
 * symptom at runtime is device_get_binding() returning NULL: the keyboard
 * keeps working and the vendor interface silently never appears. Fail the
 * build instead. The module's Kconfig raises the default to 2, but an
 * application .conf that pins the symbol lower would otherwise win quietly. */
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT >= 2,
             "CONFIG_ZMK_RAW_TOUCH_USB needs a second USB HID interface. "
             "Set CONFIG_USB_HID_DEVICE_COUNT=2 (or more) in your .conf.");

/* The full v4 input report (report ID + 11-byte body = 12 bytes) still fits
 * one interrupt IN packet at the default CONFIG_HID_INTERRUPT_EP_MPS of 16,
 * so a frame never straddles a USB (micro)frame boundary. Keep it that way:
 * a multi-packet report would also change the in_ready/hid_sem pacing
 * assumptions below. */
BUILD_ASSERT(sizeof(struct zmk_raw_touch_report) <= CONFIG_HID_INTERRUPT_EP_MPS,
             "Raw touch input report no longer fits a single interrupt IN packet");

/* HID_0 is ZMK's keyboard/consumer/mouse interface; ours is the next one. */
#define ZMK_RAW_TOUCH_USB_HID_DEV "HID_1"

static const struct device *hid_dev;

/* Exactly one in-flight interrupt IN transfer at a time. Taken by whoever
 * arms a transfer and given back only when there is nothing left to send:
 * see the transmit queue below. */
static K_SEM_DEFINE(hid_sem, 1, 1);

/* 1 while an armed transfer owns hid_sem. Whoever clears it - the
 * transfer's completion (in_ready_cb), or a path that stops waiting for
 * that completion (usb_txq_reclaim()) - takes over hid_sem, by atomic
 * compare-and-set, so exactly one of them does: a completion that arrives
 * after the semaphore was reclaimed finds nothing to take over and only
 * nudges the queue. */
static atomic_t tx_in_flight;

/* ---------------------------------------------------------------------
 * Transmit queue
 *
 * The interrupt IN endpoint carries one transfer at a time, so a frame
 * produced while the previous transfer is still in flight has to wait -
 * which two pads sharing this interface, or a peripheral pad's frames
 * arriving from the split link in bursts, make an ordinary occurrence.
 * Frames wait in the ring shared with the BLE transport (raw_touch_txq.h,
 * which states why a release must not be lost). The queue is drained by
 * transfer completion - in_ready_cb arms the next transfer rather than
 * giving the semaphore back - so it empties at exactly the rate the host
 * polls the endpoint, and a release cannot be lost while the bus is
 * healthy.
 *
 * The pacing assumption the BUILD_ASSERT above rests on holds: one
 * transfer, of one packet, in flight at a time. The queue only decides
 * what the NEXT transfer carries; it never arms a second one.
 *
 * Entries carry RT_TXQ_NO_BINDING. Unlike a BLE profile, USB has one bus
 * and one host, and the events that invalidate queued frames (bus reset,
 * detach) invalidate all of them at once, so the queue is flushed whole
 * rather than per destination.
 * ------------------------------------------------------------------ */

RT_TXQ_DEFINE(usb_txq, CONFIG_ZMK_RAW_TOUCH_USB_QUEUE_SIZE);

/*
 * The DMA source for hid_int_ep_write(). Static, never a stack buffer: on
 * nRF, usb_dc_ep_write() hands the pointer to nrfx_usbd_ep_transfer() and the
 * DMA reads it *after* the call returns. A stack buffer produces a report
 * with a plausible header and whatever the stack holds by then -- typically
 * RAM pointers -- in the tail.
 *
 * Reuse of this buffer is guarded by hid_sem, given back only from
 * in_ready_cb once the queue is empty (transfer complete, nothing more to
 * send), on a failed write, or -- via usb_conn_state_listener below --
 * once the bus leaves the HID state and the controller has abandoned the
 * transfer, so at most one in-flight transfer ever reads it.
 *
 * It is also the frame in hand (raw_touch_txq.h): a frame is popped into
 * it before its write, and a write that fails leaves it there, marked
 * tx_pending, to be written again by the next send attempt rather than
 * lost. tx_report, tx_pending and tx_session belong to the hid_sem holder.
 */
static struct zmk_raw_touch_report tx_report;
static bool tx_pending;

/* Bumped whenever the queued frames are abandoned (usb_txq_abandon()). A
 * pending frame popped before that belongs to the abandoned bus and is
 * dropped instead of written. Read before the pop, so a bump that races
 * the pop can only drop a frame, never keep a stale one. */
static atomic_t usb_session;
static atomic_val_t tx_session;

/* Arm a transfer with the pending frame, or else the frame at the head of
 * the queue, or give the semaphore back if there is nothing left to send.
 *
 * The caller must hold hid_sem, and that is precisely what makes writing
 * tx_report safe here: holding it means no transfer that could still be
 * reading the buffer is outstanding. */
static int usb_txq_send_next(void) {
    if (tx_pending && tx_session != atomic_get(&usb_session)) {
        tx_pending = false;
    }

    if (!tx_pending) {
        atomic_val_t session = atomic_get(&usb_session);
        struct rt_txq_entry entry;

        while (!rt_txq_pop(&usb_txq, &entry)) {
            k_sem_give(&hid_sem);

            /* A producer that queued between the pop and the give found
             * the semaphore taken and left its frame to us. Take the
             * semaphore back and send it - unless the queue is still
             * empty, or someone else has already taken the semaphore and
             * will. */
            if (rt_txq_is_empty(&usb_txq) || k_sem_take(&hid_sem, K_NO_WAIT) != 0) {
                return 0;
            }
        }

        tx_report.report_id = ZMK_RAW_TOUCH_REPORT_ID;
        tx_report.body = entry.body;
        tx_session = session;
    }

    /* Both set before the write, not after: the completion of a write that
     * succeeds can run before this function returns, and must find the
     * frame already sent and the transfer marked in flight. */
    tx_pending = false;
    atomic_set(&tx_in_flight, 1);

    int err = hid_int_ep_write(hid_dev, (const uint8_t *)&tx_report, sizeof(tx_report), NULL);
    if (err && atomic_cas(&tx_in_flight, 1, 0)) {
        /* Nothing was armed, so no completion will come to drain the rest
         * of the queue: keep the frame for the next attempt, hand the
         * semaphore back and let the next frame (or the bus-state
         * listener) restart it. (If the compare-and-set fails, a reclaim
         * raced the write and has already handed the semaphore on; the
         * frame belongs to a bus that is going away.) */
        tx_pending = true;
        k_sem_give(&hid_sem);
    }

    return err;
}

/* Send the head of the queue if the endpoint is idle. The take is checked:
 * a busy semaphore means a transfer is still reading tx_report, and its
 * completion drains the queue instead. */
static int usb_txq_kick(void) {
    if (k_sem_take(&hid_sem, K_NO_WAIT) != 0) {
        return 0;
    }

    return usb_txq_send_next();
}

/* Transfer complete: the endpoint is free and nothing is reading
 * tx_report any more, so the next queued frame goes out immediately with
 * the semaphore held across the handover.
 *
 * The legacy USB device stack calls this from its own callback context.
 * On nRF -- the tested platform -- that is the driver's work-queue thread,
 * never an ISR, and nothing on this path blocks, so arming the next
 * transfer from here is legal. A controller whose class callbacks are
 * dispatched from an ISR would need this deferred to a work item instead:
 * the ring is ISR-safe (spinlock), but hid_int_ep_write() is not
 * documented to be. */
static void in_ready_cb(const struct device *dev) {
    ARG_UNUSED(dev);

    if (!atomic_cas(&tx_in_flight, 1, 0)) {
        /* A transfer whose semaphore was reclaimed completed after all
         * (usb_txq_reclaim()): the endpoint is free now, so start the
         * queue the ordinary way. */
        usb_txq_kick();
        return;
    }

    int err = usb_txq_send_next();

    if (err) {
        LOG_DBG("Failed to arm the next queued raw touch frame: %d", err);
    }
}

/* Drop every frame not yet armed: the queue, and through usb_session the
 * pending frame. Their releases are lost deliberately - they belong to a
 * bus that is gone or asleep, and the host's silence watchdog closes that
 * gesture. */
static void usb_txq_discard(void) {
    atomic_inc(&usb_session);
    rt_txq_flush(&usb_txq);
}

/* Stop waiting for the armed transfer's completion and hand its semaphore
 * on, if a transfer is armed; exactly once, against the completion itself
 * (tx_in_flight). Only for a bus on which that transfer's data was taken
 * long ago (see usb_conn_state_listener()), so nothing is still reading
 * tx_report. */
static void usb_txq_reclaim(void) {
    if (atomic_cas(&tx_in_flight, 1, 0)) {
        k_sem_give(&hid_sem);
    }
}

/* The bus has gone or gone to sleep with frames queued: discard them, then
 * reclaim the semaphore of a transfer that may never complete. Discarding
 * first means a late in_ready_cb finds nothing to push onto the new bus. */
static void usb_txq_abandon(void) {
    usb_txq_discard();
    usb_txq_reclaim();
}

/* Bus-state recovery for hid_sem and the queue.
 *
 * Leaving ZMK_USB_CONN_HID (detach, bus reset, error; the same condition
 * raw_touch_lease.c drops the USB lease on) abandons any armed transfer
 * (usb_txq_abandon()). Nothing else would notice: after a detach ZMK
 * sends to BLE, so no USB send runs.
 *
 * A suspend abandons too. Nothing queued survives it: by the time the
 * host resumes the bus the frames describe a touch from before it slept,
 * and a leasing host re-reads the pad afresh after resume anyway. And a
 * transfer armed when the bus suspended is not guaranteed a completion
 * after the resume, so its semaphore is reclaimed rather than waited for -
 * otherwise USB frames would stop until a replug. If its completion does
 * come after the resume, tx_in_flight turns it into a plain kick, and a
 * write attempted while the controller still holds the old transfer fails
 * and keeps its frame pending for that kick. Reclaiming at suspend is
 * safe because nothing is written while the bus is suspended, and the
 * armed transfer's data was handed to the controller when it was armed.
 *
 * Every other status change within the HID state (resume included) kicks
 * the queue.
 *
 * The semaphore is never reclaimed on a timer while the bus is awake: an
 * armed transfer has no deadline while a configured host is not polling,
 * and reusing tx_report under it could corrupt a frame the host can still
 * collect. */
static int usb_conn_state_listener(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->conn_state != ZMK_USB_CONN_HID) {
        usb_txq_abandon();
    } else if (zmk_usb_get_status() == USB_DC_SUSPEND) {
        usb_txq_abandon();
    } else {
        usb_txq_kick();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_raw_touch_usb_hid, usb_conn_state_listener);
ZMK_SUBSCRIPTION(zmk_raw_touch_usb_hid, zmk_usb_conn_state_changed);

#define HID_GET_REPORT_TYPE_MASK 0xff00
#define HID_GET_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_INPUT 0x100
#define HID_REPORT_TYPE_FEATURE 0x300

/* Over the control pipe the report ID is part of the payload, so both
 * reports are handed back whole rather than as bodies. A request for any
 * other report gets -ENOTSUP like the Zephyr subsys gives (7.2.1 of the HID
 * v1.11 spec is unclear about reports that do not exist). */
static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    uint16_t type = setup->wValue & HID_GET_REPORT_TYPE_MASK;
    uint8_t id = setup->wValue & HID_GET_REPORT_ID_MASK;

    if (id != ZMK_RAW_TOUCH_REPORT_ID) {
        LOG_DBG("Unsupported report ID %d requested (type 0x%x)", id, type);
        return -ENOTSUP;
    }

    switch (type) {
    case HID_REPORT_TYPE_FEATURE:
        *data = (uint8_t *)zmk_raw_touch_hid_get_feature_report();
        *len = sizeof(struct zmk_raw_touch_feature_report);
        return 0;
    case HID_REPORT_TYPE_INPUT:
        *data = (uint8_t *)zmk_raw_touch_hid_get_last_report();
        *len = sizeof(struct zmk_raw_touch_report);
        return 0;
    default:
        LOG_DBG("Unsupported report type 0x%x requested", type);
        return -ENOTSUP;
    }
}

/* SET_REPORT(FEATURE) carries the protocol's host-to-device commands: the
 * host lease and tap confirm (see src/raw_touch_lease.c). Everything else
 * is still rejected with -ENOTSUP, exactly as Zephyr's default handler
 * would.
 *
 * Per HID 1.11 a control-pipe report on a device using report IDs is
 * ID-prefixed, matching what get_report_cb returns; but host stacks are
 * not uniform about the prefix on Set_Report, so a leading report-ID byte
 * is stripped when present, whatever the length, and a bare body is
 * accepted too. The two forms cannot collide: the command bytes (0x01,
 * 0x02) are distinct from the report ID 0x04. Zero padding to the feature
 * report's declared length (Windows always sends it) is accepted by the
 * command handler.
 *
 * CONFIG_ENABLE_HID_INT_OUT_EP stays untouched -- it is a global symbol
 * that would add an interrupt OUT endpoint to ZMK's keyboard interface as
 * well, and the control pipe is plenty for a lease renewal every few
 * seconds and a confirm per tap. */
static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if ((setup->wValue & HID_GET_REPORT_TYPE_MASK) != HID_REPORT_TYPE_FEATURE) {
        LOG_DBG("Unsupported report type 0x%x written", setup->wValue & HID_GET_REPORT_TYPE_MASK);
        return -ENOTSUP;
    }

    if ((setup->wValue & HID_GET_REPORT_ID_MASK) != ZMK_RAW_TOUCH_REPORT_ID) {
        LOG_DBG("Unsupported feature report ID %d written", setup->wValue & HID_GET_REPORT_ID_MASK);
        return -ENOTSUP;
    }

    const uint8_t *body = *data;
    size_t body_len = *len;

    if (body_len > 0 && body[0] == ZMK_RAW_TOUCH_REPORT_ID) {
        body++;
        body_len--;
    }

    struct zmk_endpoint_instance source = {.transport = ZMK_TRANSPORT_USB};

    return zmk_raw_touch_host_command(source, body, body_len);
}

static const struct hid_ops ops = {
    .int_in_ready = in_ready_cb,
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

int zmk_raw_touch_usb_send_report(const struct zmk_raw_touch_report_body *body) {
    if (hid_dev == NULL) {
        return -ENODEV;
    }

    switch (zmk_usb_get_status()) {
    case USB_DC_ERROR:
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_UNKNOWN:
        /* Defense in depth: usb_conn_state_listener() does this too, but
         * the event behind it is raised from a work item, so a send racing
         * the status change can get here first. Abandoning twice is
         * harmless. */
        usb_txq_abandon();
        return -ENODEV;
    default:
        break;
    }

    if (zmk_usb_get_status() == USB_DC_SUSPEND) {
        /* No transfer can complete on a suspended bus, and a frame held
         * until the host resumes it would be stale (see
         * usb_conn_state_listener()): ask the host to resume, and drop the
         * frame. A host that has not enabled remote wakeup refuses the
         * request. Abandoning again here covers a suspend whose status
         * event the listener never saw (ZMK coalesces status events that
         * arrive before its work item runs); repeating it is harmless. */
        usb_txq_abandon();
        int err = usb_wakeup_request();

        if (err) {
            LOG_DBG("USB remote wakeup request failed: %d", err);
        }
        return -EAGAIN;
    }

    /* Queue first, then try to take the endpoint - never the other way
     * round. Taking first and enqueueing only on failure loses the frame
     * whenever the in-flight transfer completes in between: in_ready_cb
     * would find the queue still empty, give the semaphore back, and the
     * frame would sit there with nothing left to drain it. For a release
     * frame there is no next frame to nudge it out, which is exactly the
     * loss this queue exists to prevent. */
    int err = rt_txq_enqueue(&usb_txq, body, RT_TXQ_NO_BINDING, "USB");

    if (err) {
        return err;
    }

    /* K_NO_WAIT, not a timeout: this runs inline on the input dispatch
     * path, so waiting here head-of-line blocks pointer deltas and taps
     * whenever USB degrades in a way the status switch above does not
     * catch. With the endpoint free the head goes out - this frame unless
     * in_ready_cb had already emptied the queue past it - so frames stay
     * in the order they were sampled. */
    return usb_txq_kick();
}

static int raw_touch_usb_hid_init(void) {
    hid_dev = device_get_binding(ZMK_RAW_TOUCH_USB_HID_DEV);
    if (hid_dev == NULL) {
        LOG_ERR("Unable to locate %s; raw touch frames will not be sent over USB. "
                "Is CONFIG_USB_HID_DEVICE_COUNT >= 2?",
                ZMK_RAW_TOUCH_USB_HID_DEV);
        return -EINVAL;
    }

    usb_hid_register_device(hid_dev, zmk_raw_touch_report_desc, zmk_raw_touch_report_desc_size,
                            &ops);

    /* No usb_hid_set_proto_code(): this interface has no boot protocol, so
     * bInterfaceProtocol stays 0 (None) and the host's boot/report protocol
     * selection on ZMK's keyboard interface does not affect it. */
    usb_hid_init(hid_dev);

    return 0;
}

SYS_INIT(raw_touch_usb_hid_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
