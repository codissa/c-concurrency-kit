#include "ck/spsc_ring.h"

#include <stdlib.h>

ck_spsc_t *ck_spsc_create(size_t capacity)
{
    size_t cap = ck_next_pow2(capacity);
    if (cap == 0)
        return NULL;

    ck_spsc_t *q = aligned_alloc(CK_CACHELINE, ck_align_up(sizeof(*q), CK_CACHELINE));
    if (!q)
        return NULL;

    q->slots = calloc(cap, sizeof(void *));
    if (!q->slots) {
        free(q);
        return NULL;
    }
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    q->cached_head = 0;
    q->cached_tail = 0;
    q->capacity = cap;
    q->mask = cap - 1;
    return q;
}

void ck_spsc_destroy(ck_spsc_t *q)
{
    if (!q)
        return;
    free(q->slots);
    free(q);
}
