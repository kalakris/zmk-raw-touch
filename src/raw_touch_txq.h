/*
 * Copyright (c) 2026 Mrinal Kalakrishnan
 *
 * SPDX-License-Identifier: MIT
 *
 * The transmit ring both transports queue frames in
 * (src/raw_touch_hog.c and src/raw_touch_usb_hid.c).
 *
 * Internal to the module - deliberately under src/ rather than include/,
 * because nothing outside the two transports may depend on it.
 *
 * A hand-rolled ring under a spinlock rather than a K_MSGQ, because
 * RELEASE frames (ZMK_RAW_TOUCH_FLAGS_TOUCHED clear: the lift-off frame,
 * and the synthetic one emitted when a lease lapses mid-touch) need
 * guarantees a msgq cannot express. The producer never re-sends a frame,
 * and losing a release leaves the host holding a phantom finger-down
 * until its own ~150 ms silence watchdog fires, which costs the gesture
 * its lift-off momentum and can join a quick re-touch onto it. This is the
 * one place those guarantees are stated; the transports and headers point
 * here.
 *
 *  1. Never evicted. A msgq that is full can only be made room in by
 *     dropping its head, which is whatever frame happens to be oldest -
 *     quite possibly the other pad's release. Here a full queue evicts
 *     the oldest MOTION frame instead (stale position data nobody will
 *     miss: the next frame is ~10 ms behind it and carries an absolute
 *     position, so nothing is reconstructed from what is dropped). Only
 *     when every queued entry is a release does an incoming frame get
 *     dropped instead.
 *  2. Ordering. One FIFO for all pads, drained strictly head-first. Per-pad
 *     ordering therefore holds for free - a release never overtakes earlier
 *     motion of the same pad, and the next touch's frames never overtake
 *     the release that closed the previous one. Eviction removes an
 *     interior entry and closes the gap, which preserves the relative
 *     order of everything else.
 *  3. Kept until delivered. A transport takes a frame out of the ring
 *     (rt_txq_pop) when it starts transmitting it, and keeps it in hand -
 *     outside the ring, where no eviction or discard can reach it - until
 *     the transmission succeeds or the transport gives it up for a reason
 *     of its own (see each transport). Because the frame in hand is older
 *     than everything still queued and is finished before the next one is
 *     taken, holding it keeps the order of 2.
 *
 * `binding` scopes an entry to the destination it was sampled for. BLE
 * stores the active profile index, so a frame queued for one host is
 * discarded rather than delivered to another after a profile switch (its
 * `lease_held` bit belongs to the endpoint it was sampled for; see
 * zmk/raw_touch/lease.h). USB has one bus and one host, so it queues
 * everything with RT_TXQ_NO_BINDING.
 *
 * Every entry point takes the ring's own spinlock: the producer runs on
 * the input dispatch thread while the drains run on the module's BLE work
 * queue and in USB callback context, and a spinlock (which locks
 * interrupts) is the only cheap primitive valid in all three.
 *
 * Includers declare the module's log module before including this.
 */

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/raw_touch/hid.h>

/* Binding for a transport with nothing to bind to: USB has a single bus
 * and a single host. Never equal to a BLE profile index, which is >= 0. */
#define RT_TXQ_NO_BINDING ((int16_t)(-1))

struct rt_txq_entry {
    struct zmk_raw_touch_report_body body;

    /* The destination this frame was sampled for; see the block comment. */
    int16_t binding;
};

struct rt_txq {
    struct rt_txq_entry *buf;
    uint8_t size;  /* entries in buf */
    uint8_t head;  /* slot holding the oldest queued frame */
    uint8_t count; /* frames queued */

    struct k_spinlock lock;
};

/* Define a ring and its storage. `queue_size` comes from Kconfig, whose
 * own `range 2 255` this re-asserts at the point of use - head and count
 * are uint8_t. */
#define RT_TXQ_DEFINE(name, queue_size)                                                            \
    BUILD_ASSERT((queue_size) >= 2 && (queue_size) <= 255,                                         \
                 "raw touch transmit queue size must be in [2, 255]");                             \
    static struct rt_txq_entry name##_entries[queue_size];                                         \
    static struct rt_txq name = {.buf = name##_entries, .size = (queue_size)}

enum rt_txq_put_result {
    RT_TXQ_PUT_OK,      /* queued, nothing dropped */
    RT_TXQ_PUT_EVICTED, /* queued after evicting the oldest motion frame */
    RT_TXQ_PUT_FULL,    /* nothing evictable; the incoming frame was dropped */
};

static inline bool rt_txq_is_release(const struct zmk_raw_touch_report_body *frame) {
    return (frame->flags & ZMK_RAW_TOUCH_FLAGS_TOUCHED) == 0;
}

/* Ring slot holding the frame `offset` places behind the head. Callers
 * keep offset < size - count may equal size, but rt_txq_put() addresses
 * slot `count` only after evicting below it - and head < size, so one
 * subtraction wraps. */
static inline uint8_t rt_txq_slot(const struct rt_txq *q, uint8_t offset) {
    uint16_t i = (uint16_t)q->head + offset;

    return (uint8_t)(i >= q->size ? i - q->size : i);
}

/* Remove the entry at `pos` places behind the head, closing the gap.
 * Caller holds q->lock. */
static inline void rt_txq_remove_at(struct rt_txq *q, uint8_t pos) {
    if (pos == 0) {
        q->head = rt_txq_slot(q, 1);
        q->count--;
        return;
    }

    for (uint8_t i = pos; i + 1 < q->count; i++) {
        q->buf[rt_txq_slot(q, i)] = q->buf[rt_txq_slot(q, i + 1)];
    }

    q->count--;
}

static inline enum rt_txq_put_result
rt_txq_put(struct rt_txq *q, const struct zmk_raw_touch_report_body *body, int16_t binding) {
    enum rt_txq_put_result res = RT_TXQ_PUT_OK;
    k_spinlock_key_t key = k_spin_lock(&q->lock);

    if (q->count == q->size) {
        int victim = -1;

        for (uint8_t i = 0; i < q->count; i++) {
            if (!rt_txq_is_release(&q->buf[rt_txq_slot(q, i)].body)) {
                victim = i;
                break;
            }
        }

        if (victim < 0) {
            /* Every queued entry is a release frame: the queue is
             * mis-sized for the number of pads. Drop the incoming frame
             * rather than a release that a host is waiting on. */
            k_spin_unlock(&q->lock, key);
            return RT_TXQ_PUT_FULL;
        }

        rt_txq_remove_at(q, (uint8_t)victim);
        res = RT_TXQ_PUT_EVICTED;
    }

    q->buf[rt_txq_slot(q, q->count)].body = *body;
    q->buf[rt_txq_slot(q, q->count)].binding = binding;
    q->count++;

    k_spin_unlock(&q->lock, key);

    return res;
}

/* rt_txq_put() for a transport's send path, logging what the put cost.
 * `transport` names the queue in the log, and its size symbol as
 * CONFIG_ZMK_RAW_TOUCH_<transport>_QUEUE_SIZE. Returns 0 if the frame was
 * queued, or -ENOBUFS if it was dropped. */
static inline int rt_txq_enqueue(struct rt_txq *q, const struct zmk_raw_touch_report_body *body,
                                 int16_t binding, const char *transport) {
    enum rt_txq_put_result res = rt_txq_put(q, body, binding);

    if (res == RT_TXQ_PUT_FULL) {
        LOG_WRN("Raw touch %s queue (%d) full of undelivered release frames; dropped an "
                "incoming %s frame for pad %u. Raise CONFIG_ZMK_RAW_TOUCH_%s_QUEUE_SIZE.",
                transport, q->size, rt_txq_is_release(body) ? "release" : "motion", body->pad_id,
                transport);
        return -ENOBUFS;
    }

    if (res == RT_TXQ_PUT_EVICTED) {
        LOG_DBG("Raw touch %s queue full; evicted the oldest motion frame", transport);
    }

    return 0;
}

/* Take the oldest frame out of the ring, into the caller's hand. */
static inline bool rt_txq_pop(struct rt_txq *q, struct rt_txq_entry *out) {
    bool have = false;
    k_spinlock_key_t key = k_spin_lock(&q->lock);

    if (q->count > 0) {
        *out = q->buf[q->head];
        rt_txq_remove_at(q, 0);
        have = true;
    }

    k_spin_unlock(&q->lock, key);

    return have;
}

static inline bool rt_txq_is_empty(struct rt_txq *q) {
    k_spinlock_key_t key = k_spin_lock(&q->lock);
    bool empty = q->count == 0;

    k_spin_unlock(&q->lock, key);

    return empty;
}

/* Discard everything. For the events that make every queued frame
 * meaningless at once: a USB bus reset, detach or suspend. */
static inline void rt_txq_flush(struct rt_txq *q) {
    k_spinlock_key_t key = k_spin_lock(&q->lock);

    q->head = 0;
    q->count = 0;

    k_spin_unlock(&q->lock, key);
}

/* Discard the entries whose binding is (match) or is not (!match)
 * `binding`, leaving the others in order. One compaction pass: each kept
 * entry moves down to the next free position, which never lies ahead of
 * the one being read. */
static inline void rt_txq_discard_where(struct rt_txq *q, int16_t binding, bool match) {
    k_spinlock_key_t key = k_spin_lock(&q->lock);
    uint8_t kept = 0;

    for (uint8_t i = 0; i < q->count; i++) {
        const struct rt_txq_entry entry = q->buf[rt_txq_slot(q, i)];

        if ((entry.binding == binding) != match) {
            q->buf[rt_txq_slot(q, kept++)] = entry;
        }
    }

    q->count = kept;

    k_spin_unlock(&q->lock, key);
}

/* Discard the entries queued for one destination: a BLE profile whose
 * connection is gone, while another profile's frames may still be live. */
static inline void rt_txq_discard_binding(struct rt_txq *q, int16_t binding) {
    rt_txq_discard_where(q, binding, true);
}

/* Discard the entries queued for every destination but one: the BLE
 * profile that has just become active. */
static inline void rt_txq_keep_only_binding(struct rt_txq *q, int16_t binding) {
    rt_txq_discard_where(q, binding, false);
}
