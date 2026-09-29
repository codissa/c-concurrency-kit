#ifndef CK_SPINLOCK_H
#define CK_SPINLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "ck/common.h"

/*
 * Test-and-test-and-set spinlock with bounded exponential backoff.
 *
 * Waiters spin on a plain load (which stays in their local cache in Shared
 * state) and only attempt the RMW when the lock looks free. This avoids the
 * cache-line ping-pong a naive TAS lock produces under contention.
 */
typedef struct {
    atomic_bool locked;
} ck_spinlock_t;

#define CK_SPINLOCK_INIT { false }

static inline void ck_spin_init(ck_spinlock_t *l)
{
    atomic_init(&l->locked, false);
}

static inline bool ck_spin_trylock(ck_spinlock_t *l)
{
    return !atomic_load_explicit(&l->locked, memory_order_relaxed) &&
           !atomic_exchange_explicit(&l->locked, true, memory_order_acquire);
}

static inline void ck_spin_lock(ck_spinlock_t *l)
{
    unsigned backoff = 1;
    for (;;) {
        if (!atomic_exchange_explicit(&l->locked, true, memory_order_acquire))
            return;
        while (atomic_load_explicit(&l->locked, memory_order_relaxed)) {
            for (unsigned i = 0; i < backoff; i++)
                ck_cpu_relax();
            if (backoff < 1024)
                backoff <<= 1;
        }
    }
}

static inline void ck_spin_unlock(ck_spinlock_t *l)
{
    atomic_store_explicit(&l->locked, false, memory_order_release);
}

/*
 * Ticket lock: FIFO-fair spinlock. Each acquirer takes a ticket and waits
 * until "now serving" reaches it, so no thread can starve.
 */
typedef struct {
    _Atomic uint32_t next;
    _Atomic uint32_t owner;
} ck_ticket_lock_t;

#define CK_TICKET_LOCK_INIT { 0, 0 }

static inline void ck_ticket_init(ck_ticket_lock_t *l)
{
    atomic_init(&l->next, 0);
    atomic_init(&l->owner, 0);
}

static inline void ck_ticket_lock(ck_ticket_lock_t *l)
{
    uint32_t me = atomic_fetch_add_explicit(&l->next, 1, memory_order_relaxed);
    while (atomic_load_explicit(&l->owner, memory_order_acquire) != me)
        ck_cpu_relax();
}

static inline void ck_ticket_unlock(ck_ticket_lock_t *l)
{
    /* Only the holder writes owner, so a plain load + store is enough. */
    uint32_t cur = atomic_load_explicit(&l->owner, memory_order_relaxed);
    atomic_store_explicit(&l->owner, cur + 1, memory_order_release);
}

#endif /* CK_SPINLOCK_H */
