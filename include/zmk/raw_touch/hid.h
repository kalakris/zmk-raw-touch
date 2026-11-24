/*
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * Wire format for raw touch frames (protocol v4).
 *
 * docs/protocol.md is the normative description of this
 * layout; the structs here are its C spelling and must match it byte for
 * byte (src/raw_touch_hid.c BUILD_ASSERTs the sizes).
 *
 * The module owns a private HID report descriptor containing exactly one
 * top-level vendor-defined application collection, carried on its own USB
 * HID interface and its own HID-over-GATT service instance. Nothing here
 * touches ZMK's own descriptor, which is why no fork of ZMK is needed.
 */

#pragma once

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/raw_touch/version.h>

/* Protocol identity, fixed rather than Kconfig-tunable: hosts match the
 * application collection by usage page/usage and then the report ID, so a
 * different value would be silently invisible to every host. */
#define ZMK_RAW_TOUCH_USAGE_PAGE 0xFF00
#define ZMK_RAW_TOUCH_USAGE 0x01
#define ZMK_RAW_TOUCH_REPORT_ID 0x04

/* Input report: one 11-byte frame, emitted per pad sample while touched
 * and a host lease is held (~100 Hz), plus exactly one release frame
 * (touched = 0, z = 0) on lift-off. */

/* Touched. Clear marks a RELEASE frame - the one emitted at lift-off, and
 * the single synthetic one emitted when a host lease lapses mid-touch.
 * Release frames are the frames whose delivery matters, and both
 * transports protect them on the way out (src/raw_touch_txq.h explains how
 * and why). Motion frames may be dropped under pressure; `seq` exposes
 * that. */
#define ZMK_RAW_TOUCH_FLAGS_TOUCHED BIT(0)
#define ZMK_RAW_TOUCH_FLAGS_SCROLL_MODE BIT(1)
/* Lease held: set iff the endpoint this frame is being sent to held a
 * live host lease when the frame was sampled, i.e. the scroll-context
 * wheel fallback is suppressed for it. Since frames are only emitted
 * while a lease is held, this is implied-set on ordinary frames; the one
 * frame carrying it clear is the single synthetic release emitted when a
 * lease lapses mid-touch. Hosts synthesize scroll only when this and
 * SCROLL_MODE are both set, making wheel and synthesized scroll mutually
 * exclusive by construction (see zmk/raw_touch/lease.h). */
#define ZMK_RAW_TOUCH_FLAGS_LEASE_HELD BIT(2)
/* Relayed: the frame's pad is on a split peripheral, so its samples reached
 * the central through a zmk,input-split relay. Clear for a pad on the half
 * that talks to the host, and on every frame of a non-split keyboard. */
#define ZMK_RAW_TOUCH_FLAGS_RELAYED BIT(3)
/* Relay wireless: only with RELAYED. Set when the split transport that
 * delivered the frame's samples to the central was the Bluetooth one,
 * clear when it was the wired one. */
#define ZMK_RAW_TOUCH_FLAGS_RELAY_WIRELESS BIT(4)
/* Bits 5-7 are reserved and sent as zero. */

struct zmk_raw_touch_report_body {
    uint8_t pad_id;
    uint8_t contact_id; /* 0 on single-touch pads */
    uint16_t x;         /* little-endian, raw pad counts */
    uint16_t y;         /* little-endian, raw pad counts */
    uint8_t z;          /* touch strength */
    uint8_t flags;      /* ZMK_RAW_TOUCH_FLAGS_* */
    uint8_t seq;        /* per-pad counter, +1 per emitted report, wraps */
    uint16_t timestamp; /* little-endian, 100 us units, wraps at 6.5536 s */
} __packed;

struct zmk_raw_touch_report {
    uint8_t report_id;
    struct zmk_raw_touch_report_body body;
} __packed;

/* Feature report: self-describing pad capabilities, on the same report ID.
 * Readable over USB GET_REPORT and the BLE feature report characteristic.
 *
 * Its body is 16 + 8 * N bytes, where N = ZMK_RAW_TOUCH_FEATURE_PAD_SLOTS
 * is the number of pads compiled into this firmware - 32 bytes on the
 * two-pad reference build (33 over USB, where the control transfer
 * carries the report ID as its first byte). Hosts MUST accept any body
 * length of that form and MUST NOT hard-code 32.
 *
 * Hosts MUST read and validate this before treating a vendor collection on
 * this usage page as the raw touch protocol - 0xFF00/0x01 is a commonly
 * squatted vendor pair. Validation means all three of: the magic below,
 * protocol_version, and a body length of the 16 + 8 * N shape. */

#define ZMK_RAW_TOUCH_PROTOCOL_VERSION 4

/* Identification magic: the ASCII bytes "RAWT", at a fixed offset in the
 * feature report body. This is what makes an unrelated device on the
 * generic 0xFF00/0x01 vendor pair distinguishable from a raw touch
 * keyboard, so a host MUST reject a body whose magic does not match and
 * MUST NOT write a lease command to such a device. Identification, not
 * authentication: it carries no secret and proves nothing about who is
 * talking. */
#define ZMK_RAW_TOUCH_MAGIC_0 'R'
#define ZMK_RAW_TOUCH_MAGIC_1 'A'
#define ZMK_RAW_TOUCH_MAGIC_2 'W'
#define ZMK_RAW_TOUCH_MAGIC_3 'T'
#define ZMK_RAW_TOUCH_MAGIC_INIT                                                                   \
    {ZMK_RAW_TOUCH_MAGIC_0, ZMK_RAW_TOUCH_MAGIC_1, ZMK_RAW_TOUCH_MAGIC_2, ZMK_RAW_TOUCH_MAGIC_3}

/* Device id: the SoC's own hardware identifier, from Zephyr's
 * hwinfo_get_device_id() (the FICR DEVICEID pair on an nRF52), byte order
 * exactly as hwinfo returns it. Eight bytes because that is what the
 * supported SoCs report; a shorter answer is zero-padded on the right, a
 * longer one truncated, and all-zero means "this build could not tell"
 * (no HWINFO driver for the SoC).
 *
 * It is a stable per-chip identifier, so a host can recognize the SAME
 * keyboard across transports - USB and Bluetooth expose no common
 * identifier otherwise. Readable by any host that can read the feature
 * report, which on BLE means a bonded one: an identifier, NOT a secret,
 * and nothing may be authorized on the strength of it. Hosts MUST NOT
 * reject a device over this field's value, including all-zero. */
#define ZMK_RAW_TOUCH_DEVICE_ID_LEN 8

/* Capabilities bit 0: host lease supported - the host may acquire a lease
 * on the stream by writing the feature report (see zmk/raw_touch/lease.h),
 * switching the pads from standard (firmware-driven) scrolling to
 * host-driven scrolling. Advertised whenever a host-facing transport is
 * built; hosts MUST check this bit before acquiring a lease. */
#define ZMK_RAW_TOUCH_CAP_HOST_LEASE BIT(0)

/* Capabilities bit 1: tap confirm supported - the firmware accepts the
 * ZMK_RAW_TOUCH_CMD_TAP_CONFIRM feature command (see zmk/raw_touch/tap.h).
 * Advertised with the lease; a host MUST check it before writing one. */
#define ZMK_RAW_TOUCH_CAP_TAP_CONFIRM BIT(1)

/* Pad slot byte +1. Bits 0-2 are the pad's mounting; bit 3 says the pad
 * parks its scroll-context taps for host confirmation while a lease is
 * held (tap-click together with tap-click-while-scrolling on the node). */
#define ZMK_RAW_TOUCH_ORIENT_ROTATE_90 BIT(0)
#define ZMK_RAW_TOUCH_ORIENT_X_INVERT BIT(1)
#define ZMK_RAW_TOUCH_ORIENT_Y_INVERT BIT(2)
#define ZMK_RAW_TOUCH_PAD_PARKS_SCROLL_TAPS BIT(3)

/* One slot per pad compiled in, so the report is exactly as long as the
 * hardware needs and no longer. Derived from the devicetree rather than
 * from a Kconfig knob or a fixed cap: the pad instances are what
 * src/raw_touch.c enumerates, so the two can never disagree. pad-ids are
 * unique and below 8 (asserted at build time in src/raw_touch.c and
 * src/raw_touch_hid.c), so there are at most 8 pads and every one has a
 * slot.
 *
 * Floor of 1 keeps struct zmk_raw_touch_feature_body well-formed (and
 * the HID descriptor's feature REPORT_COUNT nonzero) if this header is
 * ever pulled into a build with no pad node.
 *
 * WARNING: changing the pad count changes the report body's length,
 * which changes the HID report descriptor - and macOS caches the HOGP
 * report map at pairing time. Adding or removing a pad therefore needs a
 * forget + re-pair on the host (see the README's Troubleshooting; the
 * failure is deceptively partial - USB fine, BLE frames unparseable). */
#define ZMK_RAW_TOUCH_FEATURE_PAD_SLOTS MAX(DT_NUM_INST_STATUS_OKAY(zmk_raw_touch_pad), 1)

/* The feature report's pads_present mask, from the devicetree: bit N set
 * if a pad node has pad-id N. An enum constant rather than a macro, so it
 * can be used inside another DT_FOREACH_STATUS_OKAY() expansion, where
 * the preprocessor would refuse to expand that macro a second time. */
#define ZMK_RAW_TOUCH_PAD_BIT_(node_id) | BIT(DT_PROP(node_id, pad_id))
enum {
    ZMK_RAW_TOUCH_PADS_PRESENT =
        (uint8_t)(0 DT_FOREACH_STATUS_OKAY(zmk_raw_touch_pad, ZMK_RAW_TOUCH_PAD_BIT_)),
};

/* The feature slot describing the pad with @p pad_id: slots list the
 * present pads in ascending pad-id order, so it is the number of present
 * pads with a lower id. An integer constant expression. */
#define ZMK_RAW_TOUCH_POPCOUNT8_(x)                                                                \
    ((((x) >> 0) & 1) + (((x) >> 1) & 1) + (((x) >> 2) & 1) + (((x) >> 3) & 1) +                   \
     (((x) >> 4) & 1) + (((x) >> 5) & 1) + (((x) >> 6) & 1) + (((x) >> 7) & 1))
#define ZMK_RAW_TOUCH_FEATURE_SLOT(pad_id)                                                         \
    ZMK_RAW_TOUCH_POPCOUNT8_((ZMK_RAW_TOUCH_PADS_PRESENT) & (BIT(pad_id) - 1))

/* Whether a zmk,raw-touch-pad node's device is a zmk,input-split relay
 * rather than a pad the central can see directly, i.e. whether the half
 * that owns the pad is the peripheral. That decides whose module version
 * the pad's feature slot reports. */
#define ZMK_RAW_TOUCH_PAD_IS_RELAYED(node_id)                                                      \
    DT_NODE_HAS_COMPAT(DT_PHANDLE(node_id, device), zmk_input_split)

struct zmk_raw_touch_feature_pad_slot {
    uint8_t resolution;   /* counts/mm, 0 = unknown */
    uint8_t orientation;  /* ZMK_RAW_TOUCH_ORIENT_* | ZMK_RAW_TOUCH_PAD_* flags */
    uint16_t x_max;       /* little-endian */
    uint16_t y_max;       /* little-endian */
    uint8_t max_contacts; /* 1 on a Pinnacle */
    /* Packed module version (zmk/raw_touch/version.h) of the half that
     * OWNS this pad - this one for a local pad, the peripheral's own for a
     * relayed pad, which announces it over the split link (0 until it
     * has; see zmk/raw_touch/split_stamp.h). */
    uint8_t module_version;
} __packed;

struct zmk_raw_touch_feature_body {
    uint8_t protocol_version; /* ZMK_RAW_TOUCH_PROTOCOL_VERSION */
    uint8_t pads_present;     /* bit N set if pad-id N exists */
    uint8_t capabilities;     /* ZMK_RAW_TOUCH_CAP_* */
    /* Packed module version of the half answering this report, i.e. the
     * central. Diagnostic only: protocol_version above is what a host
     * keys its parsing off. */
    uint8_t module_version;
    /* ZMK_RAW_TOUCH_MAGIC_*, the ASCII bytes "RAWT". Fixed for the life
     * of the protocol; protocol_version stays at byte 0 ahead of it so a
     * host can read the version before it knows the layout. */
    uint8_t magic[4];
    /* ZMK_RAW_TOUCH_DEVICE_ID_LEN bytes of hwinfo device id; all-zero
     * when this build could not read one. Diagnostic and identifying,
     * never a compatibility or admission check. */
    uint8_t device_id[ZMK_RAW_TOUCH_DEVICE_ID_LEN];
    /* Present pads in ascending pad-id order, one slot per pad compiled
     * in. Hosts recover N from the report length as (len - 16) / 8 and
     * read min(N, popcount(pads_present)) slots. */
    struct zmk_raw_touch_feature_pad_slot pads[ZMK_RAW_TOUCH_FEATURE_PAD_SLOTS];
} __packed;

struct zmk_raw_touch_feature_report {
    uint8_t report_id;
    struct zmk_raw_touch_feature_body body;
} __packed;

/* The module's private report descriptor, defined in src/raw_touch_hid.c. */
extern const uint8_t zmk_raw_touch_report_desc[];
extern const size_t zmk_raw_touch_report_desc_size;

/* The last input report produced, for a host that reads the input report
 * rather than receiving it (USB GET_REPORT(INPUT), a BLE read of the input
 * characteristic). Frames are sent from the producer's own copy; this one
 * only answers those reads (src/raw_touch_hid.c). */
void zmk_raw_touch_hid_set_last_report(const struct zmk_raw_touch_report_body *body);
struct zmk_raw_touch_report *zmk_raw_touch_hid_get_last_report(void);

/* The feature report is built from the devicetree at compile time
 * (src/raw_touch_hid.c); only these two fields are set at run time. */

/* Copies up to ZMK_RAW_TOUCH_DEVICE_ID_LEN bytes of hwinfo device id into
 * the feature report, zero-padding a shorter id and ignoring the tail of a
 * longer one. Called once at init; the field is constant thereafter. */
void zmk_raw_touch_hid_set_feature_device_id(const uint8_t *device_id, size_t len);
/* A relayed pad's owning half announces its version over the split link,
 * which happens long after init has filled the slots - so that one byte
 * alone can be set again later. */
void zmk_raw_touch_hid_set_feature_slot_version(int slot, uint8_t module_version);
struct zmk_raw_touch_feature_report *zmk_raw_touch_hid_get_feature_report(void);
