#ifndef CK_SLAB_H
#define CK_SLAB_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Lock-free fixed-size object pool.
 *
 * All objects live in one contiguous, cache-line aligned region allocated up
 * front, so alloc/free never touch the system allocator. Free objects form a
 * Treiber stack of 32-bit indices. The stack head packs {tag:32, index:32}
 * into one 64-bit word and every successful CAS bumps the tag, which defeats
 * the ABA problem without needing a double-width CAS.
 *
 * The "next" links are kept in a side array rather than inside the free
 * objects, so a racing reader never reads memory that a user may be writing.
 */
typedef struct ck_slab {
    _Atomic uint64_t head;
    _Atomic uint32_t *next;
    unsigned char *mem;
    size_t obj_size;
    uint32_t capacity;
    _Atomic uint32_t in_use;
} ck_slab_t;

/* Returns NULL on failure or if capacity >= UINT32_MAX. */
ck_slab_t *ck_slab_create(size_t obj_size, uint32_t capacity);
void ck_slab_destroy(ck_slab_t *slab);

/* Returns NULL when the pool is exhausted. */
void *ck_slab_alloc(ck_slab_t *slab);

/* ptr must have come from ck_slab_alloc on the same slab. */
void ck_slab_free(ck_slab_t *slab, void *ptr);

/* Number of objects currently handed out (approximate under concurrency). */
uint32_t ck_slab_in_use(ck_slab_t *slab);

#endif /* CK_SLAB_H */
