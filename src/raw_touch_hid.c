/*
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * The module's private HID report descriptor and report state.
 *
 * The descriptor is a single top-level vendor-defined application collection
 * carrying one Input report (a touch frame) and one Feature report (pad
 * capabilities), both on ZMK_RAW_TOUCH_REPORT_ID. It is served verbatim on
 * our own USB HID interface (src/raw_touch_usb_hid.c) and our own
 * HID-over-GATT report map characteristic (src/raw_touch_hog.c); ZMK's own
 * descriptor is never touched.
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/hid.h>

#include <zephyr/logging/log.h>

/* The module's LOG_MODULE_REGISTER lives in src/raw_touch_log.c. */
LOG_MODULE_DECLARE(zmk_raw_touch, CONFIG_ZMK_RAW_TOUCH_LOG_LEVEL);

#include <zmk/raw_touch/hid.h>

/* Zephyr's <zephyr/usb/class/hid.h> has HID_USAGE_PAGE() (1-byte payload)
 * but no 2-byte form, which a vendor-defined page such as 0xFF00 needs. */
#ifndef HID_USAGE_PAGE16
#define HID_USAGE_PAGE16(a, b) HID_ITEM(HID_ITEM_TAG_USAGE_PAGE, HID_ITEM_TYPE_GLOBAL, 2), a, b
#endif

/* HID main-item flags: Data (bit 0 clear), Variable (bit 1 set), Absolute
 * (bit 2 clear). Spelled out locally rather than pulled in as ZMK's
 * ZMK_HID_MAIN_VAL_* macros, which live in <zmk/hid.h> alongside a
 * file-scope definition of ZMK's own report descriptor. */
#define RAW_TOUCH_HID_DATA_VAR_ABS 0x02

/* Data usages within the vendor collection. Part of the wire protocol (the
 * host matches on the collection's usage page/usage, then on report ID), so
 * these are fixed rather than Kconfig-tunable. */
#define RAW_TOUCH_HID_USAGE_INPUT 0x02
#define RAW_TOUCH_HID_USAGE_FEATURE 0x03

BUILD_ASSERT(sizeof(struct zmk_raw_touch_report_body) == 11,
             "Raw touch input report body must stay 11 bytes; the host parses it by offset");
BUILD_ASSERT(sizeof(struct zmk_raw_touch_feature_pad_slot) == 8,
             "Raw touch feature pad slot must stay 8 bytes; the host parses it by offset");
/* The feature body is a 16-byte header plus one 8-byte slot per pad, so
 * its length is 16 + 8 * N (32 on a two-pad build). Hosts derive N from
 * the report length; nothing here may pad the struct. */
BUILD_ASSERT(sizeof(struct zmk_raw_touch_feature_body) == 16 + 8 * ZMK_RAW_TOUCH_FEATURE_PAD_SLOTS,
             "Raw touch feature report body must stay 16 + 8 * pads bytes; "
             "the host parses it by offset");
/* The header's fixed offsets, which hosts parse by. protocol_version is
 * at 0 in every version of the protocol by definition, so it needs no
 * assert of its own. */
BUILD_ASSERT(offsetof(struct zmk_raw_touch_feature_body, magic) == 4,
             "Raw touch feature magic must stay at body offset 4");
BUILD_ASSERT(offsetof(struct zmk_raw_touch_feature_body, device_id) == 8,
             "Raw touch feature device id must stay at body offset 8");
BUILD_ASSERT(offsetof(struct zmk_raw_touch_feature_body, pads) == 16,
             "Raw touch feature pad slots must start at body offset 16");
/* pads_present is an 8-bit mask with one bit per pad-id, and every pad
 * gets a slot; src/raw_touch.c asserts that each pad-id is below 8. */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(zmk_raw_touch_pad) <= 8, "At most 8 raw touch pads");
BUILD_ASSERT(ZMK_RAW_TOUCH_POPCOUNT8_(ZMK_RAW_TOUCH_PADS_PRESENT) ==
                 DT_NUM_INST_STATUS_OKAY(zmk_raw_touch_pad),
             "raw touch pad-ids must be unique: two pads share one");
/* The descriptor's feature REPORT_COUNT below is a single-byte HID item
 * payload, so the body must fit in a byte; 8 pads make 80 bytes. */
BUILD_ASSERT(sizeof(struct zmk_raw_touch_feature_body) <= 0xFF,
             "Raw touch feature report body no longer fits a one-byte HID REPORT_COUNT");

/* The descriptor declares the pads' REAL logical ranges on the x/y fields,
 * so generic HID tooling sees true geometry without parsing the feature
 * report. The values come straight from the zmk,raw-touch-pad node: this
 * file only compiles under CONFIG_ZMK_RAW_TOUCH, which depends on
 * DT_HAS_ZMK_RAW_TOUCH_PAD_ENABLED, so a status-okay node always exists
 * here.
 *
 * With several pads, DT_COMPAT_GET_ANY_STATUS_OKAY picks one of them: the
 * compile-time descriptor can only carry one geometry, so it describes that
 * any-instance pad and the per-pad truth lives in the feature report's
 * slots. Hosts consuming multi-pad streams must use the slots, not the
 * descriptor. */
#define RAW_TOUCH_DESC_PAD_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_raw_touch_pad)
#define RAW_TOUCH_DESC_X_MAX DT_PROP(RAW_TOUCH_DESC_PAD_NODE, x_max)
#define RAW_TOUCH_DESC_Y_MAX DT_PROP(RAW_TOUCH_DESC_PAD_NODE, y_max)

/* HID logical values are signed, so a 16-bit Logical Maximum item tops out
 * at 0x7FFF. (That is also why the 8-bit fields below use the 16-bit item
 * form for a maximum of 0xFF, and the timestamp the 32-bit form for
 * 0xFFFF.) */
BUILD_ASSERT(RAW_TOUCH_DESC_X_MAX > 0 && RAW_TOUCH_DESC_X_MAX <= 0x7FFF,
             "raw touch pad x-max must fit a 16-bit signed HID logical maximum");
BUILD_ASSERT(RAW_TOUCH_DESC_Y_MAX > 0 && RAW_TOUCH_DESC_Y_MAX <= 0x7FFF,
             "raw touch pad y-max must fit a 16-bit signed HID logical maximum");

const uint8_t zmk_raw_touch_report_desc[] = {
    HID_USAGE_PAGE16((ZMK_RAW_TOUCH_USAGE_PAGE & 0xFF), ((ZMK_RAW_TOUCH_USAGE_PAGE >> 8) & 0xFF)),
    HID_USAGE(ZMK_RAW_TOUCH_USAGE),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
    HID_REPORT_ID(ZMK_RAW_TOUCH_REPORT_ID),

    /* Input: one touch frame, 11 bytes, split into per-field items so each
     * carries its real logical range.
     * pad_id, contact_id, x (u16 LE), y (u16 LE), z, flags, seq,
     * timestamp (u16 LE). Logical Minimum is a global item, so the 0 set
     * here holds for every field below. */

    /* pad_id, contact_id: 8-bit, 0..255. */
    HID_USAGE(RAW_TOUCH_HID_USAGE_INPUT),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX16(0xFF, 0x00),
    HID_REPORT_SIZE(0x08),
    HID_REPORT_COUNT(0x02),
    HID_INPUT(RAW_TOUCH_HID_DATA_VAR_ABS),

    /* x: 16-bit, 0..x-max. */
    HID_USAGE(RAW_TOUCH_HID_USAGE_INPUT),
    HID_LOGICAL_MAX16((RAW_TOUCH_DESC_X_MAX & 0xFF), ((RAW_TOUCH_DESC_X_MAX >> 8) & 0xFF)),
    HID_REPORT_SIZE(0x10),
    HID_REPORT_COUNT(0x01),
    HID_INPUT(RAW_TOUCH_HID_DATA_VAR_ABS),

    /* y: 16-bit, 0..y-max. */
    HID_USAGE(RAW_TOUCH_HID_USAGE_INPUT),
    HID_LOGICAL_MAX16((RAW_TOUCH_DESC_Y_MAX & 0xFF), ((RAW_TOUCH_DESC_Y_MAX >> 8) & 0xFF)),
    HID_REPORT_SIZE(0x10),
    HID_REPORT_COUNT(0x01),
    HID_INPUT(RAW_TOUCH_HID_DATA_VAR_ABS),

    /* z, flags, seq: 8-bit, 0..255. */
    HID_USAGE(RAW_TOUCH_HID_USAGE_INPUT),
    HID_LOGICAL_MAX16(0xFF, 0x00),
    HID_REPORT_SIZE(0x08),
    HID_REPORT_COUNT(0x03),
    HID_INPUT(RAW_TOUCH_HID_DATA_VAR_ABS),

    /* timestamp: 16-bit, 0..65535. */
    HID_USAGE(RAW_TOUCH_HID_USAGE_INPUT),
    HID_LOGICAL_MAX32(0xFF, 0xFF, 0x00, 0x00),
    HID_REPORT_SIZE(0x10),
    HID_REPORT_COUNT(0x01),
    HID_INPUT(RAW_TOUCH_HID_DATA_VAR_ABS),

    /* Feature: self-describing pad capabilities, uniform 8-bit fields
     * (its u16 members are protocol-level structure the host parses by
     * offset; the descriptor does not model them). Readable over USB
     * GET_REPORT and the BLE feature report characteristic.
     * protocol_version, pads_present, capabilities, module_version, the
     * 4-byte magic, the 8-byte device id, then one 8-byte slot per pad
     * compiled in: 16 + 8 * N bytes, 32 on the two-pad reference build.
     *
     * The count is taken from sizeof() rather than written out, so the
     * descriptor and the struct cannot drift apart when the pad count
     * changes (which needs a re-pair: see ZMK_RAW_TOUCH_FEATURE_PAD_SLOTS). */
    HID_USAGE(RAW_TOUCH_HID_USAGE_FEATURE),
    HID_LOGICAL_MAX16(0xFF, 0x00),
    HID_REPORT_SIZE(0x08),
    HID_REPORT_COUNT(sizeof(struct zmk_raw_touch_feature_body)),
    HID_FEATURE(RAW_TOUCH_HID_DATA_VAR_ABS),

    HID_END_COLLECTION,
};

const size_t zmk_raw_touch_report_desc_size = sizeof(zmk_raw_touch_report_desc);

/* All zero until the first frame. */
static struct zmk_raw_touch_report last_report = {.report_id = ZMK_RAW_TOUCH_REPORT_ID};

void zmk_raw_touch_hid_set_last_report(const struct zmk_raw_touch_report_body *body) {
    last_report.body = *body;
}

struct zmk_raw_touch_report *zmk_raw_touch_hid_get_last_report(void) { return &last_report; }

/* One pad node's feature slot, at the index ZMK_RAW_TOUCH_FEATURE_SLOT()
 * gives its pad-id.
 *
 * max_contacts = 1: the module streams a single contact per pad
 * (contact_id 0); a Pinnacle reports one finger anyway. The version is the
 * OWNING half's: this build for a local pad, and 0 (unknown) for a relayed
 * one until its half announces its own over the split link, which
 * src/raw_touch.c then writes into the slot. */
#define RAW_TOUCH_FEATURE_SLOT_INIT(node_id)                                                       \
    [ZMK_RAW_TOUCH_FEATURE_SLOT(DT_PROP(node_id, pad_id))] = {                                     \
        .resolution = DT_PROP(node_id, resolution),                                                \
        .orientation = (DT_PROP(node_id, rotate_90) ? ZMK_RAW_TOUCH_ORIENT_ROTATE_90 : 0) |        \
                       (DT_PROP(node_id, x_invert) ? ZMK_RAW_TOUCH_ORIENT_X_INVERT : 0) |          \
                       (DT_PROP(node_id, y_invert) ? ZMK_RAW_TOUCH_ORIENT_Y_INVERT : 0) |          \
                       (DT_PROP(node_id, tap_click) && DT_PROP(node_id, tap_click_while_scrolling) \
                            ? ZMK_RAW_TOUCH_PAD_PARKS_SCROLL_TAPS                                  \
                            : 0),                                                                  \
        .x_max = sys_cpu_to_le16(DT_PROP(node_id, x_max)),                                         \
        .y_max = sys_cpu_to_le16(DT_PROP(node_id, y_max)),                                         \
        .max_contacts = 1,                                                                         \
        .module_version =                                                                          \
            ZMK_RAW_TOUCH_PAD_IS_RELAYED(node_id) ? 0 : ZMK_RAW_TOUCH_MODULE_VERSION_PACKED,       \
    },

/* Everything but the device id is known at compile time. */
static struct zmk_raw_touch_feature_report touch_feature_report = {
    .report_id = ZMK_RAW_TOUCH_REPORT_ID,
    .body = {.protocol_version = ZMK_RAW_TOUCH_PROTOCOL_VERSION,
             .pads_present = ZMK_RAW_TOUCH_PADS_PRESENT,
             /* The host lease needs a host-facing transport for its
              * writes; without one the capability must not be advertised.
              * (Moot in practice: with no transport there is no host to
              * read this report either.) */
             .capabilities = IS_ENABLED(CONFIG_ZMK_RAW_TOUCH_HOST_TRANSPORT)
                                 ? (ZMK_RAW_TOUCH_CAP_HOST_LEASE | ZMK_RAW_TOUCH_CAP_TAP_CONFIRM)
                                 : 0,
             /* This half's build. A relayed pad's slot carries its own
              * half's version instead. */
             .module_version = ZMK_RAW_TOUCH_MODULE_VERSION_PACKED,
             .magic = ZMK_RAW_TOUCH_MAGIC_INIT,
             .pads = {DT_FOREACH_STATUS_OKAY(zmk_raw_touch_pad, RAW_TOUCH_FEATURE_SLOT_INIT)}},
};

void zmk_raw_touch_hid_set_feature_device_id(const uint8_t *device_id, size_t len) {
    /* Zero first, so a hwinfo id shorter than the field is zero-padded on
     * the right and a failed read leaves the all-zero "unknown". A longer
     * id keeps its leading bytes; byte order is hwinfo's, untouched. */
    memset(touch_feature_report.body.device_id, 0, sizeof(touch_feature_report.body.device_id));

    if (device_id == NULL || len == 0) {
        return;
    }

    memcpy(touch_feature_report.body.device_id, device_id,
           MIN(len, sizeof(touch_feature_report.body.device_id)));
}

void zmk_raw_touch_hid_set_feature_slot_version(int slot, uint8_t module_version) {
    if (slot < 0 || slot >= ZMK_RAW_TOUCH_FEATURE_PAD_SLOTS) {
        LOG_ERR("Raw touch feature slot %d out of range", slot);
        return;
    }

    /* A lone byte, so a host GET racing this write reads either the old
     * value or the new one - never a torn slot. */
    touch_feature_report.body.pads[slot].module_version = module_version;
}

struct zmk_raw_touch_feature_report *zmk_raw_touch_hid_get_feature_report(void) {
    return &touch_feature_report;
}
