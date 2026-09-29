#ifndef CK_SPSC_RING_H
#define CK_SPSC_RING_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "ck/common.h"

/*
 * Wait-free single-producer / single-consumer ring buffer of pointers.
 *
 * - head is written only by the consumer, tail only by the producer; they live
 *   on separate cache lines to avoid false sharing.
 * - Each side keeps a private cached copy of the other side's index and only
 *   re-reads the shared one when the cached value says full/empty. In steady
 *   state this removes almost all cross-core cache traffic.
 * - Indices increase monotonically and are masked on access, so full and
 *   empty are distinguishable without wasting a slot.
 */
typedef struct ck_spsc {
    alignas(CK_CACHELINE) _Atomic size_t head; /* next slot to read  (consumer) */
    size_t cached_tail;                        /* consumer's view of tail       */

    alignas(CK_CACHELINE) _Atomic size_t tail; /* next slot to write (producer) */
    size_t cached_head;                        /* producer's view of head       */

    alignas(CK_CACHELINE) size_t capacity;
    size_t mask;
    void **slots;
} ck_spsc_t;

/* Capacity is rounded up to a power of two. Returns NULL on allocation failure. */
ck_spsc_t *ck_spsc_create(size_t capacity);
void ck_spsc_destroy(ck_spsc_t *q);

/* Producer side. Returns false if the ring is full. */
static inline bool ck_spsc_push(ck_spsc_t *q, void *item)
{
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (tail - q->cached_head == q->capacity) {
        q->cached_head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (tail - q->cached_head == q->capacity)
            return false;
    }
    q->slots[tail & q->mask] = item;
    /* Publish the slot write before the new tail becomes visible. */
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}

/* Consumer side. Returns false if the ring is empty. */
static inline bool ck_spsc_pop(ck_spsc_t *q, void **out)
{
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (head == q->cached_tail) {
        q->cached_tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head == q->cached_tail)
            return false;
    }
    *out = q->slots[head & q->mask];
    /* Release so the producer cannot overwrite the slot before we read it. */
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

/* Approximate: exact only when called from the producer or consumer thread. */
static inline size_t ck_spsc_size(ck_spsc_t *q)
{
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    return tail - head;
}

#endif /* CK_SPSC_RING_H */
