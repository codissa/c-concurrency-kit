#ifndef CK_MPMC_QUEUE_H
#define CK_MPMC_QUEUE_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "ck/common.h"

/*
 * Bounded lock-free multi-producer / multi-consumer queue
 * (Dmitry Vyukov's sequence-number design).
 *
 * Every cell carries a sequence number that encodes whose turn it is:
 *   seq == pos        -> cell is free for the producer that claims `pos`
 *   seq == pos + 1    -> cell holds data for the consumer that claims `pos`
 * Producers and consumers claim positions with a single CAS on their own
 * cursor and then hand the cell over with a release store on `seq`, so there
 * is exactly one contended RMW per operation and no ABA problem.
 */
typedef struct {
    _Atomic size_t seq;
    void *data;
} ck_mpmc_cell_t;

typedef struct ck_mpmc {
    alignas(CK_CACHELINE) ck_mpmc_cell_t *cells;
    size_t mask;
    alignas(CK_CACHELINE) _Atomic size_t enqueue_pos;
    alignas(CK_CACHELINE) _Atomic size_t dequeue_pos;
} ck_mpmc_t;

/* Capacity is rounded up to a power of two. Returns NULL on allocation failure. */
ck_mpmc_t *ck_mpmc_create(size_t capacity);
void ck_mpmc_destroy(ck_mpmc_t *q);

/* Returns false if the queue is full. Safe from any number of threads. */
bool ck_mpmc_push(ck_mpmc_t *q, void *item);

/* Returns false if the queue is empty. Safe from any number of threads. */
bool ck_mpmc_pop(ck_mpmc_t *q, void **out);

#endif /* CK_MPMC_QUEUE_H */
