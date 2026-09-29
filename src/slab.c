#include "ck/slab.h"

#include <stdalign.h>
#include <stdlib.h>

#include "ck/common.h"

#define SLAB_NIL UINT32_MAX

static inline uint64_t pack(uint32_t tag, uint32_t idx)
{
    return ((uint64_t)tag << 32) | idx;
}

static inline uint32_t head_idx(uint64_t h) { return (uint32_t)h; }
static inline uint32_t head_tag(uint64_t h) { return (uint32_t)(h >> 32); }

ck_slab_t *ck_slab_create(size_t obj_size, uint32_t capacity)
{
    if (obj_size == 0 || capacity == 0 || capacity == SLAB_NIL)
        return NULL;

    ck_slab_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->obj_size = ck_align_up(obj_size, alignof(max_align_t));
    s->capacity = capacity;
    s->mem = aligned_alloc(CK_CACHELINE, ck_align_up(s->obj_size * capacity, CK_CACHELINE));
    s->next = malloc(sizeof(*s->next) * capacity);
    if (!s->mem || !s->next) {
        free(s->mem);
        free(s->next);
        free(s);
        return NULL;
    }

    /* Initial free list: 0 -> 1 -> ... -> capacity-1 -> NIL */
    for (uint32_t i = 0; i < capacity; i++)
        atomic_init(&s->next[i], i + 1 < capacity ? i + 1 : SLAB_NIL);
    atomic_init(&s->head, pack(0, 0));
    atomic_init(&s->in_use, 0);
    return s;
}

void ck_slab_destroy(ck_slab_t *s)
{
    if (!s)
        return;
    free(s->mem);
    free(s->next);
    free(s);
}

void *ck_slab_alloc(ck_slab_t *s)
{
    uint64_t old = atomic_load_explicit(&s->head, memory_order_acquire);
    for (;;) {
        uint32_t idx = head_idx(old);
        if (idx == SLAB_NIL)
            return NULL;
        /* May be stale if another thread pops idx concurrently; the tag makes
         * our CAS fail in that case, so the stale value is never installed. */
        uint32_t nxt = atomic_load_explicit(&s->next[idx], memory_order_relaxed);
        uint64_t desired = pack(head_tag(old) + 1, nxt);
        if (atomic_compare_exchange_weak_explicit(&s->head, &old, desired,
                                                  memory_order_acquire,
                                                  memory_order_acquire)) {
            atomic_fetch_add_explicit(&s->in_use, 1, memory_order_relaxed);
            return s->mem + (size_t)idx * s->obj_size;
        }
    }
}

void ck_slab_free(ck_slab_t *s, void *ptr)
{
    size_t off = (size_t)((unsigned char *)ptr - s->mem);
    uint32_t idx = (uint32_t)(off / s->obj_size);

    uint64_t old = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint64_t desired;
    do {
        atomic_store_explicit(&s->next[idx], head_idx(old), memory_order_relaxed);
        desired = pack(head_tag(old) + 1, idx);
        /* Release: our writes to the object (and next[idx]) happen-before the
         * next thread that pops it. */
    } while (!atomic_compare_exchange_weak_explicit(&s->head, &old, desired,
                                                    memory_order_release,
                                                    memory_order_relaxed));
    atomic_fetch_sub_explicit(&s->in_use, 1, memory_order_relaxed);
}

uint32_t ck_slab_in_use(ck_slab_t *s)
{
    return atomic_load_explicit(&s->in_use, memory_order_relaxed);
}
