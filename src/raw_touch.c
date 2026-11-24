/*
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * Raw touch frames (protocol v4): forwards absolute touch frames
 * (position + touch strength) from a trackpad running in absolute mode to
 * the host over a vendor-defined HID report (see zmk/raw_touch/hid.h).
 *
 * Reports are emitted only while the selected endpoint's host holds a
 * live lease (see raw_touch_lease.c) - i.e. only in RawTouch mode, never
 * in Standard mode: one report per pad sample while touched (~100 Hz),
 * plus exactly one release report (touched = 0, z = 0) on lift-off.
 * Without a lease, nothing is emitted - nobody is listening, and at
 * ~100 Hz of 11-byte reports the wasted BLE airtime is real. If the lease
 * lapses mid-touch, one final synthetic release report (touched = 0,
 * flags bit 2 clear) closes the gesture of the host that saw the touch
 * before the stream goes quiet. Each frame carries a per-pad sequence
 * number (drop detection) and a device-side timestamp in 100 us units
 * (host-side velocity that BLE batching cannot distort), and a pad relayed
 * from a split peripheral flags which split link delivered it. A feature report
 * on the same report ID describes the protocol version, this build's
 * module version, the SoC's device id, the pads present and, per pad, its
 * resolution, orientation, coordinate ranges, contact count and the
 * module version of the half that owns it; its body is 16 + 8 * (pads
 * compiled in) bytes - 32 on a two-pad build - and it is readable over
 * USB GET_REPORT and the BLE HOG feature-report characteristic.
 *
 * One instance per zmk,raw-touch-pad devicetree node. Nothing here is
 * specific to any particular touchpad ASIC: the node names the input
 * device and declares the geometry, orientation and tap parameters that
 * the pad's own driver binding would otherwise have to supply.
 *
 * Scroll mode: frames carry the scroll-mode flag while a processor chain
 * containing the zmk,input-processor-raw-touch-scroll marker is the one
 * handling the pad's events (see the commentary in zmk/raw_touch/scroll.h).
 *
 * Dual mode: relative deltas derived from successive absolute positions
 * are ALWAYS re-injected as REL_X/REL_Y on the same input device,
 * regardless of scroll mode, so the existing input listener chain
 * (scaling, wheel-mapping overlays, temp mouse layer, buttons) keeps
 * working in Standard mode. A live host lease suppresses only the
 * scroll-context deltas (see below).
 *
 * Tap-to-click: when the pad node sets tap-click, a touch that lifts off
 * within tap-max-ms and never strays more than tap-max-movement raw
 * counts (Chebyshev distance) from its touch-down point - and never had a
 * scroll-mode frame, unless tap-click-while-scrolling is set - injects an
 * INPUT_BTN_0 press + release into the pad's normal input pipeline, so
 * existing button processors apply. A scroll-context tap that lifts off
 * while a host holds the lease is parked instead of emitted, until the
 * host confirms it (zmk/raw_touch/tap.h): the host is the only party that
 * knows whether the touch merely caught a coasting momentum tail.
 */

#define DT_DRV_COMPAT zmk_raw_touch_pad

#include <stdlib.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_DECLARE(zmk_raw_touch, CONFIG_ZMK_RAW_TOUCH_LOG_LEVEL);

#include <zmk/raw_touch/lease.h>
#include <zmk/raw_touch/hid.h>
#include <zmk/raw_touch/scroll.h>
#include <zmk/raw_touch/split_stamp.h>
#include <zmk/raw_touch/tap.h>
#include <zmk/raw_touch/transport.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_WIRED)
#include <zmk/split/transport/central.h>

/* With both split transports built, which one carries the peripheral's
 * events is decided at run time, and ZMK exposes it through no API. These
 * are ZMK's own definitions: the transport src/split/central.c has
 * selected, and the Bluetooth transport's registration in
 * src/split/bluetooth/central.c. Should ZMK rename or hide either, the
 * build fails to link rather than misreporting. */
extern const struct zmk_split_transport_central *active_transport;
extern struct zmk_split_transport_central bt_central;
#endif
#endif

/* The input thread's queue is what makes re-injection work: the deltas and
 * taps reported from inside a pad's input callback are dispatched after
 * the event being handled, not nested inside it, and a tap confirmed from
 * a transport's thread reaches the listener on the input thread like any
 * other event. */
BUILD_ASSERT(IS_ENABLED(CONFIG_INPUT_MODE_THREAD),
             "zmk,raw-touch-pad requires CONFIG_INPUT_MODE_THREAD");

struct raw_touch_pad_config {
    /* The input device producing the absolute touch events, and the
     * device the derived relative deltas and taps are injected back on. */
    const struct device *dev;

    uint8_t pad_id;

    /* A pad driver's own invert/rotate feed transforms typically only
     * apply to its relative mode, so mirror them here when deriving
     * pointer deltas from the raw absolute coordinates. The streamed
     * frames stay untransformed; the same property values go out verbatim
     * in the feature report's orientation byte so hosts can apply them. */
    bool rotate_90;
    bool x_invert;
    bool y_invert;

    bool tap_click;
    bool tap_click_while_scrolling;
    uint16_t tap_max_ms;
    uint16_t tap_max_movement;

    /* ZMK_RAW_TOUCH_PAD_IS_RELAYED(): the pad's half is the peripheral,
     * which announces its own module version for the pad's feature slot,
     * and the pad's frames carry the relay flags. */
    bool relayed;

    /* This pad's slot in the feature report (ZMK_RAW_TOUCH_FEATURE_SLOT()). */
    uint8_t feature_slot;
};

struct raw_touch_pad_data {
    /* The frame being accumulated until its sync event. */
    struct {
        uint16_t x, y;
        uint8_t z;
        /* An event of this frame has been seen. */
        bool open;
        /* Scroll-context marks seen while accumulating it. */
        bool scroll;
    } frame;

    /* Set by the scroll marker (zmk_raw_touch_scroll_mark()) when a chain
     * containing it handles an event from this pad's device; taken on
     * every event of the pad's own. */
    atomic_t scroll_marked;

    /* Peripheral sample times, when the pad is relayed from a split
     * peripheral running the split-stamp processor (see
     * zmk/raw_touch/split_stamp.h). */
    struct {
        /* The stamp for the frame that closes next. Latest wins; taken by
         * every frame. */
        bool pending;
        uint16_t value;
        /* The last stamp minus the local clock when its frame was
         * processed (wrapping, in the timestamp's units). A frame whose
         * stamp was lost is dated local clock + this, which keeps it on
         * the pad's one timeline in the peripheral's clock domain. 0 for a
         * pad that has never been stamped, which leaves it on the local
         * clock. */
        uint16_t offset;
    } stamp;

    /* The previous frame, for release dedup and delta derivation. Its
     * position is meaningful only while it was touched. */
    bool prev_touched;
    uint16_t prev_x, prev_y;

    /* What the host has been sent. */
    struct {
        /* +1 per emitted report, wraps. */
        uint8_t seq;
        /* The last emitted report said touched, i.e. the host currently
         * believes a finger is down. Distinct from prev_touched (the pad's
         * physical state, which keeps tracking while no report goes out):
         * a mid-touch lease lapse must emit exactly one synthetic release
         * report so the host is not left holding a phantom finger-down,
         * and this is the state that says one is owed. */
        bool touched;
        /* The endpoint index the last emitted report went to: the only
         * one a synthetic release may go to. */
        int endpoint;
    } stream;

    /* Tap detection for the current touch. */
    struct {
        /* The touch-down frame's timestamp, on the same timeline as the
         * frames, and position. */
        uint16_t down_ts;
        uint16_t down_x, down_y;
        /* The touch can still be a tap. */
        bool candidate;
        /* Some frame of this touch was in scroll context. */
        bool scrolled;
        /* k_uptime_get_32() deadline until which a parked tap accepts a
         * confirmation; 0 = none. Set on the input thread and taken by
         * zmk_raw_touch_tap_confirm() on the transport's thread. */
        atomic_t parked_until;
        /* An INPUT_BTN_0 release the input queue had no room for; retried
         * by the pad's next input callback. Set from either thread. */
        atomic_t release_owed;
    } tap;

    /* Packed module version last announced by the owning half of a relayed
     * pad; 0 until it announces. Mirrors what the slot already holds, so
     * the LOG_INF fires once per change rather than once per touch. */
    uint8_t peer_version;
};

/* Taps are reported with K_NO_WAIT on every path. On the input thread,
 * waiting for room in the input queue would deadlock the queue being
 * dispatched from; a confirmed tap arrives on the BT RX thread or in the
 * USB stack's control-request context, which must not stall either.
 * Dropping a click beats both. */

/* Release INPUT_BTN_0, or owe the release when the input queue has no
 * room: raw_touch_input_event() retries it on the pad's next callback, so
 * a press that went in is never left held. */
static void raw_touch_release_tap(const struct raw_touch_pad_config *cfg,
                                  struct raw_touch_pad_data *data) {
    if (input_report_key(cfg->dev, INPUT_BTN_0, 0, true, K_NO_WAIT) != 0) {
        atomic_set(&data->tap.release_owed, 1);
    }
}

/* Press and release are separate sync'd events, so the listener sends a
 * button-down report followed by a button-up report through the pad's
 * normal processor chain. A press that did not go in is a dropped click
 * and owes no release. */
static void raw_touch_emit_tap(const struct raw_touch_pad_config *cfg,
                               struct raw_touch_pad_data *data) {
    if (input_report_key(cfg->dev, INPUT_BTN_0, 1, true, K_NO_WAIT) == 0) {
        raw_touch_release_tap(cfg, data);
    } else {
        LOG_DBG("Raw touch pad %d tap dropped: input queue full", cfg->pad_id);
    }
}

/* Device-side sample time (HID Scan Time convention, 100 us units): hosts
 * derive finger velocity from this rather than from arrival time, which
 * BLE connection-interval batching distorts. Read at most once per frame -
 * the 64-bit divide is not free on a Cortex-M.
 *
 * This is the local pad's clock, read when the frame is processed. A pad
 * relayed from a split peripheral is processed here too, after the split
 * hop, so for it this reading would carry the link's delivery jitter; the
 * peripheral can send its own reading along with the frame instead (see
 * zmk/raw_touch/split_stamp.h), and raw_touch_frame_timestamp() prefers
 * that. */
static uint16_t raw_touch_timestamp(void) { return (uint16_t)zmk_raw_touch_split_stamp_now(); }

/* Timestamp units per millisecond. */
#define RT_TIMESTAMP_PER_MS 10

/* Take the peripheral stamp for the frame being processed, if one arrived.
 * Every frame takes it, before anything can return early: left pending, it
 * would misdate a later frame. */
static bool raw_touch_take_stamp(struct raw_touch_pad_data *data, uint16_t *stamp) {
    bool have_stamp = data->stamp.pending;

    *stamp = data->stamp.value;
    data->stamp.pending = false;

    if (have_stamp) {
        data->stamp.offset = (uint16_t)(*stamp - raw_touch_timestamp());
    }

    return have_stamp;
}

/* The timestamp of the frame being processed, on the pad's one timeline:
 * its peripheral stamp when that arrived, and otherwise the local clock
 * shifted by the last stamp's offset. A pad that has never been stamped
 * has offset 0, i.e. the local clock. */
static uint16_t raw_touch_frame_timestamp(const struct raw_touch_pad_data *data, bool have_stamp,
                                          uint16_t stamp) {
    return have_stamp ? stamp : (uint16_t)(raw_touch_timestamp() + data->stamp.offset);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Whether the split link that delivered a relayed pad's samples to the
 * central is the Bluetooth transport rather than the wired one. Only
 * called for a relayed pad, i.e. on a central with a split transport.
 *
 * With a single split transport built it is the only one there is. With
 * both, it is the transport the central has selected: the central
 * discards peripheral events arriving on any other, so the samples of a
 * frame being processed came through the selected one. The frame handler
 * runs on the input thread just after the events are delivered, so only a
 * frame still in the input queue at the instant of a switch between
 * cable and wireless can be attributed to the transport selected after it. */
static bool raw_touch_relay_wireless(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_WIRED)
    return active_transport == &bt_central;
#else
    return IS_ENABLED(CONFIG_ZMK_SPLIT_BLE);
#endif
}
#endif

/* Flags bits 3-4: whether the pad's samples were relayed from a split
 * peripheral, and over which split link. Taken from the frame being
 * processed, so every report built from it - including a synthetic
 * release - carries the link that delivered the pad's latest samples. */
static uint8_t raw_touch_relay_flags(const struct raw_touch_pad_config *cfg) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (cfg->relayed) {
        return ZMK_RAW_TOUCH_FLAGS_RELAYED |
               (raw_touch_relay_wireless() ? ZMK_RAW_TOUCH_FLAGS_RELAY_WIRELESS : 0);
    }
#else
    ARG_UNUSED(cfg);
#endif
    return 0;
}

/* Track the current touch's tap candidacy through this frame. */
static void raw_touch_tap_update(const struct raw_touch_pad_config *cfg,
                                 struct raw_touch_pad_data *data, bool touched, bool scroll_mode,
                                 uint16_t ts) {
    if (touched && !data->prev_touched) {
        /* Touch-down: start a tap candidacy. */
        data->tap.down_ts = ts;
        data->tap.down_x = data->frame.x;
        data->tap.down_y = data->frame.y;
        data->tap.candidate = cfg->tap_click;
        data->tap.scrolled = false;
        /* A new touch supersedes a parked tap the host never confirmed. */
        atomic_clear(&data->tap.parked_until);
    }

    data->tap.scrolled = data->tap.scrolled || scroll_mode;

    /* The timestamps are 16-bit and wrap every 6.5 s, which the unsigned
     * difference absorbs for any shorter touch. Longer touches are safe
     * too: a touched pad reports every few ms, so the candidacy ends on the
     * first frame past tap-max-ms (bounded well below the wrap by a
     * BUILD_ASSERT), long before the difference can wrap back into range. */
    if (data->tap.candidate &&
        (uint16_t)(ts - data->tap.down_ts) > cfg->tap_max_ms * RT_TIMESTAMP_PER_MS) {
        data->tap.candidate = false;
    }

    if (touched && data->tap.candidate) {
        int travel_x = (int)data->frame.x - (int)data->tap.down_x;
        int travel_y = (int)data->frame.y - (int)data->tap.down_y;
        if (MAX(abs(travel_x), abs(travel_y)) > cfg->tap_max_movement) {
            data->tap.candidate = false;
        }
    }
}

enum raw_touch_tap_action {
    RT_TAP_NONE,
    RT_TAP_EMIT,
    /* Held for the host to confirm (see zmk/raw_touch/tap.h). */
    RT_TAP_PARK,
};

/* What a lift-off does about a tap, decided once from the whole touch and
 * the lease as it stands at lift-off. */
static enum raw_touch_tap_action raw_touch_tap_action(const struct raw_touch_pad_config *cfg,
                                                      const struct raw_touch_pad_data *data,
                                                      bool lease_held) {
    if (!data->tap.candidate) {
        return RT_TAP_NONE;
    }

    if (!data->tap.scrolled) {
        return RT_TAP_EMIT;
    }

    /* Scroll context vetoes the tap unless the pad opts out of the veto:
     * a flick on a scroll layer, or a touch that stops momentum in
     * RawTouch mode, must not click by default, but a dedicated scrolling
     * pad may want its taps (e.g. mapped to a right click downstream). */
    if (!cfg->tap_click_while_scrolling) {
        return RT_TAP_NONE;
    }

    /* A leasing host runs the momentum, so only it knows whether the touch
     * merely caught a coasting tail: park the tap and let it decide.
     * Without the lease this is Standard mode, where taps are never
     * parked. */
    return lease_held ? RT_TAP_PARK : RT_TAP_EMIT;
}

/* Send one report for this pad to @p ep and record it as the last report
 * (what a host reading the input report sees). contact_id is 0: the module
 * streams a single contact per pad.
 *
 * The send result is deliberately not checked. Both transports QUEUE the
 * frame and protect releases on the way out (src/raw_touch_txq.h), so
 * there is nothing a per-frame producer could usefully retry: the cases
 * that still lose a frame (the bus or the link going away) are exactly the
 * cases where re-sending it would go nowhere either. */
static void raw_touch_send(const struct raw_touch_pad_config *cfg, struct raw_touch_pad_data *data,
                           struct zmk_raw_touch_endpoint ep, uint16_t x, uint16_t y, uint8_t z,
                           uint8_t flags, uint16_t ts) {
    const struct zmk_raw_touch_report_body body = {
        .pad_id = cfg->pad_id,
        .contact_id = 0,
        .x = sys_cpu_to_le16(x),
        .y = sys_cpu_to_le16(y),
        .z = z,
        .flags = flags,
        .seq = data->stream.seq++,
        .timestamp = sys_cpu_to_le16(ts),
    };

    zmk_raw_touch_hid_set_last_report(&body);
    zmk_raw_touch_send_report(ep, &body);
}

/* Stream this frame to a leasing host, or close the host's gesture once
 * the lease is gone. */
static void raw_touch_stream_emit(const struct raw_touch_pad_config *cfg,
                                  struct raw_touch_pad_data *data, struct zmk_raw_touch_endpoint ep,
                                  bool lease_held, bool touched, bool scroll_mode, uint16_t ts) {
    uint8_t relay_flags = raw_touch_relay_flags(cfg);

    if (lease_held) {
        uint8_t flags = (touched ? ZMK_RAW_TOUCH_FLAGS_TOUCHED : 0) |
                        (scroll_mode ? ZMK_RAW_TOUCH_FLAGS_SCROLL_MODE : 0) |
                        ZMK_RAW_TOUCH_FLAGS_LEASE_HELD | relay_flags;

        /* !touched implies the frame's x/y/z are all zero (that is how
         * touched is derived), so the release report's zeros need no
         * special case. */
        raw_touch_send(cfg, data, ep, data->frame.x, data->frame.y, data->frame.z, flags, ts);
        data->stream.touched = touched;
        data->stream.endpoint = ep.index;
        return;
    }

    if (!data->stream.touched) {
        /* No lease and nothing owed: emit nothing. */
        return;
    }

    /* The lease ended mid-touch (timeout, host release, endpoint switch -
     * see raw_touch_lease.c) and the last report the host saw said
     * touched. Emit exactly one synthetic release report so the host
     * closes its gesture instead of holding a phantom finger-down (runaway
     * momentum), then go silent. Bit 2 is clear - its meaning stays exact:
     * "a host lease was held when this frame was sampled" - which also
     * tells the host the wheel fallback is live again, so it must not add
     * lift-off momentum on top.
     *
     * Only to the endpoint that saw the touch: after an endpoint switch
     * the report would reach a host that never did, so nothing is sent and
     * the old host's silence watchdog closes the gesture. */
    if (ep.index == data->stream.endpoint) {
        raw_touch_send(cfg, data, ep, 0, 0, 0,
                       (scroll_mode ? ZMK_RAW_TOUCH_FLAGS_SCROLL_MODE : 0) | relay_flags, ts);
    }
    data->stream.touched = false;
}

/* Dual mode: derive relative deltas for the normal pointer pipeline from
 * successive absolute positions, even in scroll mode - an existing
 * wheel-mapping overlay then provides standard wheel scrolling for hosts
 * that never lease. */
static void raw_touch_fallback_deltas(const struct raw_touch_pad_config *cfg,
                                      const struct raw_touch_pad_data *data) {
    int32_t raw_dx = (int32_t)data->frame.x - (int32_t)data->prev_x;
    int32_t raw_dy = (int32_t)data->frame.y - (int32_t)data->prev_y;

    int32_t dx = cfg->rotate_90 ? raw_dy : raw_dx;
    int32_t dy = cfg->rotate_90 ? raw_dx : raw_dy;
    if (cfg->x_invert) {
        dx = -dx;
    }
    if (cfg->y_invert) {
        dy = -dy;
    }

    if (dx != 0 || dy != 0) {
        /* K_NO_WAIT: dropping a delta beats deadlocking the input queue we
         * are dispatched from. */
        input_report_rel(cfg->dev, INPUT_REL_X, dx, false, K_NO_WAIT);
        input_report_rel(cfg->dev, INPUT_REL_Y, dy, true, K_NO_WAIT);
    }
}

/* This frame becomes the previous one. */
static void raw_touch_commit(struct raw_touch_pad_data *data, bool touched) {
    if (touched) {
        data->prev_x = data->frame.x;
        data->prev_y = data->frame.y;
    } else {
        data->tap.candidate = false;
    }
    data->prev_touched = touched;
}

static void raw_touch_process_frame(const struct raw_touch_pad_config *cfg,
                                    struct raw_touch_pad_data *data, bool scroll_mode) {
    uint16_t stamp;
    bool have_stamp = raw_touch_take_stamp(data, &stamp);

    /* An absolute-mode pad marks lift-off with an all-zeros idle frame. */
    bool touched = !(data->frame.x == 0 && data->frame.y == 0 && data->frame.z == 0);

    if (!touched && !data->prev_touched) {
        /* Nothing to stream while idle; also swallows any extra idle
         * frames so exactly one release report goes out. */
        return;
    }

    /* The endpoint this frame goes to, and whether its host holds a live
     * lease, read once and used for everything below: the frame's flags,
     * its destination, the endpoint a synthetic release is checked
     * against, and the wheel suppression all describe the same endpoint.
     * Deliberately NOT latched across frames - frame emission, the flag
     * and the wheel suppression must all revert on the very next frame
     * after a lease lapses or the endpoint switches away. */
    struct zmk_raw_touch_endpoint ep = zmk_raw_touch_selected_endpoint();
    bool lease_held = zmk_raw_touch_lease_held(ep.index);

    /* The frame's sample time. It also times taps, so that for a relayed
     * pad the split link's delivery bunching cannot stretch or shorten a
     * touch. */
    uint16_t ts = raw_touch_frame_timestamp(data, have_stamp, stamp);

    raw_touch_tap_update(cfg, data, touched, scroll_mode, ts);

    enum raw_touch_tap_action tap =
        touched ? RT_TAP_NONE : raw_touch_tap_action(cfg, data, lease_held);

    if (tap == RT_TAP_PARK) {
        /* Before the release frame goes out, so the tap is already parked
         * by the time the host can confirm it. */
        atomic_set(&data->tap.parked_until,
                   (atomic_val_t)(k_uptime_get_32() + ZMK_RAW_TOUCH_TAP_CONFIRM_WINDOW_MS));
    }

    raw_touch_stream_emit(cfg, data, ep, lease_held, touched, scroll_mode, ts);

    /* Everything from here on - relative deltas, taps, position tracking -
     * runs whether or not a host holds the lease, because it IS Standard
     * mode (cursor + tap + wheel fallback).
     *
     * The lease suppresses exactly the scroll-context deltas: while the
     * selected endpoint holds a lease they are not injected at all, so the
     * wheel-mapping overlay downstream has nothing to emit and the leasing
     * host (which is synthesizing scroll from the frames) never sees
     * doubled scrolling. Pointer-context deltas, taps and every key path
     * are untouched, and the position keeps being tracked so the first
     * delta after the lease ends is an ordinary one-frame step, not a
     * jump. Hosts that never lease are unaffected: the wheel keeps
     * working. */
    if (touched && data->prev_touched && !(lease_held && scroll_mode)) {
        raw_touch_fallback_deltas(cfg, data);
    }

    if (tap == RT_TAP_EMIT) {
        raw_touch_emit_tap(cfg, data);
    }

    raw_touch_commit(data, touched);
}

/* The module version of the half that owns a relayed pad, announced ahead
 * of each touch's first frame (see zmk/raw_touch/split_stamp.h) and
 * republished in that pad's feature report slot, so a host reading the
 * report sees both halves' builds. Ignored for a local pad, whose slot
 * already carries this half's version: nothing should be able to talk a
 * central out of its own number. */
static void raw_touch_set_peer_version(const struct raw_touch_pad_config *cfg,
                                       struct raw_touch_pad_data *data, uint8_t version) {
    if (!cfg->relayed || version == data->peer_version) {
        return;
    }

    data->peer_version = version;
    LOG_INF("Raw touch pad %d is relayed from a half running module version %d.%d", cfg->pad_id,
            version >> 4, version & 0x0F);

    zmk_raw_touch_hid_set_feature_slot_version(cfg->feature_slot, version);
}

static void raw_touch_input_event(const struct raw_touch_pad_config *cfg,
                                  struct raw_touch_pad_data *data, struct input_event *evt) {
    /* Every callback for the pad, whatever the event, is a chance to send
     * an owed tap release - the first is usually the tap's own press
     * coming back through the queue, which has freed a slot. */
    if (atomic_clear(&data->tap.release_owed)) {
        raw_touch_release_tap(cfg, data);
    }

    if (evt->type == ZMK_RAW_TOUCH_SPLIT_STAMP_TYPE) {
        switch (evt->code) {
        case ZMK_RAW_TOUCH_SPLIT_STAMP_CODE:
            /* The peripheral's sample time, sent just ahead of the frame's
             * sync (see zmk/raw_touch/split_stamp.h). Latest wins: if the
             * stamped frame's own sync were lost on the link, the next
             * frame's stamp supersedes this one instead of the stale one
             * misdating that frame. */
            data->stamp.value = (uint16_t)evt->value;
            data->stamp.pending = true;
            break;
        case ZMK_RAW_TOUCH_SPLIT_VERSION_CODE:
            raw_touch_set_peer_version(cfg, data, (uint8_t)evt->value);
            break;
        }
        return;
    }

    if (evt->type != INPUT_EV_ABS) {
        /* Ignore everything else, including our own injected REL/KEY events. */
        return;
    }

    switch (evt->code) {
    case INPUT_ABS_X:
        data->frame.x = (uint16_t)evt->value;
        break;
    case INPUT_ABS_Y:
        data->frame.y = (uint16_t)evt->value;
        break;
    case INPUT_ABS_Z:
        data->frame.z = (uint8_t)evt->value;
        break;
    default:
        return;
    }

    /* Fold the scroll-context latch into the frame being accumulated (see
     * zmk/raw_touch/scroll.h for why the latch is already correct on the
     * first frame of a touch, whichever order the input callbacks run in).
     *
     * The latch is discarded rather than accumulated on the first event of
     * a frame. A mark can be set *after* we handled the previous sync (if
     * the listener's callback runs after ours, or from the relative deltas
     * we injected), and carrying that forward would flag the first frame
     * of the next touch with the scroll state of the previous one. Since
     * the discard happens before the second event of the frame is even
     * dispatched, no mark belonging to this frame can be lost with it.
     */
    bool marked = atomic_clear(&data->scroll_marked);

    if (data->frame.open) {
        data->frame.scroll = data->frame.scroll || marked;
    } else {
        /* First event of a frame: `marked` belongs to the previous one. */
        data->frame.scroll = false;
        data->frame.open = true;
    }

    /* A frame ends on its sync event. That is the Z event (see the
     * README's driver requirements), and Z is checked as well because the
     * idle filter clears the sync flag of an event the listener must not
     * report: when the listener's callback runs before this one, the frame's
     * last event arrives here without the flag. */
    if (evt->sync || evt->code == INPUT_ABS_Z) {
        data->frame.open = false;
        raw_touch_process_frame(cfg, data, data->frame.scroll);
    }
}

/* Every property is checked against the field that stores it, so an
 * out-of-range value fails the build instead of being truncated: the
 * feature slot stores resolution in one byte and the ranges in 16 bits,
 * and any pad may be the one the HID descriptor declares its signed
 * 16-bit logical maxima from (raw_touch_hid.c). pad-id uniqueness is
 * asserted across all pads in raw_touch_hid.c. */
#define RT_INST(n)                                                                                 \
    BUILD_ASSERT(DT_INST_PROP(n, pad_id) >= 0 && DT_INST_PROP(n, pad_id) < 8,                      \
                 "raw touch pad-id must be between 0 and 7");                                      \
    BUILD_ASSERT(DT_INST_PROP(n, tap_max_ms) >= 0 && DT_INST_PROP(n, tap_max_ms) <= 5000,          \
                 "raw touch tap-max-ms must be between 0 and 5000");                               \
    BUILD_ASSERT(DT_INST_PROP(n, tap_max_movement) >= 0 &&                                         \
                     DT_INST_PROP(n, tap_max_movement) <= 0x7FFF,                                  \
                 "raw touch tap-max-movement must be between 0 and 32767");                        \
    BUILD_ASSERT(!DT_INST_PROP(n, tap_click_while_scrolling) || DT_INST_PROP(n, tap_click),        \
                 "raw touch tap-click-while-scrolling requires tap-click");                        \
    BUILD_ASSERT(DT_INST_PROP(n, resolution) >= 0 && DT_INST_PROP(n, resolution) <= 255,           \
                 "raw touch resolution must be between 0 and 255");                                \
    BUILD_ASSERT(DT_INST_PROP(n, x_max) > 0 && DT_INST_PROP(n, x_max) <= 0x7FFF,                   \
                 "raw touch x-max must be between 1 and 32767");                                   \
    BUILD_ASSERT(DT_INST_PROP(n, y_max) > 0 && DT_INST_PROP(n, y_max) <= 0x7FFF,                   \
                 "raw touch y-max must be between 1 and 32767");                                   \
    static struct raw_touch_pad_data rt_data_##n;                                                  \
    static const struct raw_touch_pad_config rt_config_##n = {                                     \
        .dev = DEVICE_DT_GET(DT_INST_PHANDLE(n, device)),                                          \
        .pad_id = DT_INST_PROP(n, pad_id),                                                         \
        .rotate_90 = DT_INST_PROP(n, rotate_90),                                                   \
        .x_invert = DT_INST_PROP(n, x_invert),                                                     \
        .y_invert = DT_INST_PROP(n, y_invert),                                                     \
        .tap_click = DT_INST_PROP(n, tap_click),                                                   \
        .tap_click_while_scrolling = DT_INST_PROP(n, tap_click_while_scrolling),                   \
        .tap_max_ms = DT_INST_PROP(n, tap_max_ms),                                                 \
        .tap_max_movement = DT_INST_PROP(n, tap_max_movement),                                     \
        .relayed = ZMK_RAW_TOUCH_PAD_IS_RELAYED(DT_DRV_INST(n)),                                   \
        .feature_slot = ZMK_RAW_TOUCH_FEATURE_SLOT(DT_INST_PROP(n, pad_id)),                       \
    };                                                                                             \
    static void rt_input_cb_##n(struct input_event *evt) {                                         \
        raw_touch_input_event(&rt_config_##n, &rt_data_##n, evt);                                  \
    }                                                                                              \
    INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST_PHANDLE(n, device)), rt_input_cb_##n);

DT_INST_FOREACH_STATUS_OKAY(RT_INST)

#define RT_CONFIG_REF(n) &rt_config_##n,
#define RT_DATA_REF(n) &rt_data_##n,

/* Every pad, for the entry points that arrive from outside a pad's own
 * input callback. Parallel arrays: same pad, same index. */
static const struct raw_touch_pad_config *const pad_configs[] = {
    DT_INST_FOREACH_STATUS_OKAY(RT_CONFIG_REF)};
static struct raw_touch_pad_data *const pad_data[] = {DT_INST_FOREACH_STATUS_OKAY(RT_DATA_REF)};

/* Runs in the input thread, from inside a listener's processor chain. A
 * device that feeds no pad (a marked chain on some other pointing device)
 * matches nothing and is ignored. */
void zmk_raw_touch_scroll_mark(const struct device *dev) {
    for (size_t i = 0; i < ARRAY_SIZE(pad_configs); i++) {
        if (pad_configs[i]->dev == dev) {
            atomic_set(&pad_data[i]->scroll_marked, 1);
        }
    }
}

int zmk_raw_touch_tap_confirm(uint8_t pad_id) {
    for (size_t i = 0; i < ARRAY_SIZE(pad_configs); i++) {
        const struct raw_touch_pad_config *cfg = pad_configs[i];
        struct raw_touch_pad_data *data = pad_data[i];

        if (cfg->pad_id != pad_id) {
            continue;
        }

        /* Taken, not read, so a parked tap is emitted at most once. */
        uint32_t until = (uint32_t)atomic_clear(&data->tap.parked_until);

        if (until != 0 && (int32_t)(until - k_uptime_get_32()) >= 0) {
            LOG_DBG("Raw touch pad %d tap confirmed by the host", pad_id);
            raw_touch_emit_tap(cfg, data);
        } else {
            LOG_DBG("Raw touch pad %d tap confirm with nothing parked", pad_id);
        }

        return 0;
    }

    LOG_WRN("Tap confirm for unknown raw touch pad %d", pad_id);
    return -EINVAL;
}

static int raw_touch_init(void) {
    /* The SoC's own id, published so a host can tell that the keyboard it
     * sees over USB and the one it sees over Bluetooth are the same
     * physical keyboard - the two transports share no other identifier.
     * Read once: it cannot change while this build is running.
     *
     * The buffer is the field's own width, so hwinfo can never return
     * more than fits; a SoC whose id is longer is truncated to its leading
     * bytes by hwinfo itself. A negative return (-ENOSYS on a SoC with no
     * HWINFO driver, or a driver-level failure) leaves the field all-zero,
     * which is the wire's "unknown" - not an error worth failing init
     * over, since nothing but host-side convenience depends on it. */
    uint8_t device_id[ZMK_RAW_TOUCH_DEVICE_ID_LEN];
    ssize_t device_id_len = hwinfo_get_device_id(device_id, sizeof(device_id));

    if (device_id_len > 0) {
        zmk_raw_touch_hid_set_feature_device_id(device_id, (size_t)device_id_len);
    } else {
        LOG_WRN("No hardware device id (%d); the feature report will report it as unknown",
                (int)device_id_len);
    }

    return 0;
}

SYS_INIT(raw_touch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
