#include "ck/mpmc_queue.h"

#include <stdint.h>
#include <stdlib.h>

ck_mpmc_t *ck_mpmc_create(size_t capacity)
{
    size_t cap = ck_next_pow2(capacity);
    if (cap == 0)
        return NULL;

    ck_mpmc_t *q = aligned_alloc(CK_CACHELINE, ck_align_up(sizeof(*q), CK_CACHELINE));
    if (!q)
        return NULL;

    q->cells = aligned_alloc(CK_CACHELINE, ck_align_up(cap * sizeof(ck_mpmc_cell_t), CK_CACHELINE));
    if (!q->cells) {
        free(q);
        return NULL;
    }
    for (size_t i = 0; i < cap; i++) {
        atomic_init(&q->cells[i].seq, i);
        q->cells[i].data = NULL;
    }
    q->mask = cap - 1;
    atomic_init(&q->enqueue_pos, 0);
    atomic_init(&q->dequeue_pos, 0);
    return q;
}

void ck_mpmc_destroy(ck_mpmc_t *q)
{
    if (!q)
        return;
    free(q->cells);
    free(q);
}

bool ck_mpmc_push(ck_mpmc_t *q, void *item)
{
    ck_mpmc_cell_t *cell;
    size_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->cells[pos & q->mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;

        if (diff == 0) {
            /* Cell is free for this lap: try to claim position `pos`. */
            if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
            /* CAS failure reloaded `pos`; retry. */
        } else if (diff < 0) {
            /* Cell still holds data from the previous lap: queue is full. */
            return false;
        } else {
            /* Another producer claimed this position; catch up. */
            pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        }
    }

    cell->data = item;
    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
    return true;
}

bool ck_mpmc_pop(ck_mpmc_t *q, void **out)
{
    ck_mpmc_cell_t *cell;
    size_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->cells[pos & q->mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->dequeue_pos, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if (diff < 0) {
            /* Producer has not filled this cell yet: queue is empty. */
            return false;
        } else {
            pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        }
    }

    *out = cell->data;
    /* Mark the cell free for the producer one lap ahead. */
    atomic_store_explicit(&cell->seq, pos + q->mask + 1, memory_order_release);
    return true;
}
