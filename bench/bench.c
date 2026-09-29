/*
 * Throughput micro-benchmarks:
 *   1. SPSC ring vs Vyukov MPMC queue vs mutex-protected ring (1P/1C .. 4P/4C)
 *   2. TAS spinlock vs ticket lock vs pthread_mutex under contention
 *
 * usage: bench [items_per_producer]
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ck/mpmc_queue.h"
#include "ck/spinlock.h"
#include "ck/spsc_ring.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------- baseline: mutex ring buffer --- */

typedef struct {
    pthread_mutex_t mu;
    void **slots;
    size_t cap, head, tail;
} mutex_ring_t;

static mutex_ring_t *mutex_ring_create(size_t cap)
{
    mutex_ring_t *r = calloc(1, sizeof(*r));
    pthread_mutex_init(&r->mu, NULL);
    r->slots = calloc(cap, sizeof(void *));
    r->cap = cap;
    return r;
}

static void mutex_ring_destroy(mutex_ring_t *r)
{
    pthread_mutex_destroy(&r->mu);
    free(r->slots);
    free(r);
}

static bool mutex_ring_push(mutex_ring_t *r, void *v)
{
    pthread_mutex_lock(&r->mu);
    bool ok = r->tail - r->head < r->cap;
    if (ok)
        r->slots[r->tail++ % r->cap] = v;
    pthread_mutex_unlock(&r->mu);
    return ok;
}

static bool mutex_ring_pop(mutex_ring_t *r, void **out)
{
    pthread_mutex_lock(&r->mu);
    bool ok = r->head != r->tail;
    if (ok)
        *out = r->slots[r->head++ % r->cap];
    pthread_mutex_unlock(&r->mu);
    return ok;
}

/* ------------------------------------------------ generic queue harness --- */

typedef struct {
    const char *name;
    void *(*create)(size_t cap);
    void (*destroy)(void *q);
    bool (*push)(void *q, void *v);
    bool (*pop)(void *q, void **out);
    int max_threads_per_side; /* 1 for SPSC */
} queue_ops_t;

static void *spsc_create_(size_t c) { return ck_spsc_create(c); }
static void spsc_destroy_(void *q) { ck_spsc_destroy(q); }
static bool spsc_push_(void *q, void *v) { return ck_spsc_push(q, v); }
static bool spsc_pop_(void *q, void **o) { return ck_spsc_pop(q, o); }
static void *mpmc_create_(size_t c) { return ck_mpmc_create(c); }
static void mpmc_destroy_(void *q) { ck_mpmc_destroy(q); }
static bool mpmc_push_(void *q, void *v) { return ck_mpmc_push(q, v); }
static bool mpmc_pop_(void *q, void **o) { return ck_mpmc_pop(q, o); }
static void *mring_create_(size_t c) { return mutex_ring_create(c); }
static void mring_destroy_(void *q) { mutex_ring_destroy(q); }
static bool mring_push_(void *q, void *v) { return mutex_ring_push(q, v); }
static bool mring_pop_(void *q, void **o) { return mutex_ring_pop(q, o); }

static const queue_ops_t QUEUES[] = {
    { "spsc_ring", spsc_create_, spsc_destroy_, spsc_push_, spsc_pop_, 1 },
    { "mpmc_queue", mpmc_create_, mpmc_destroy_, mpmc_push_, mpmc_pop_, 64 },
    { "mutex_ring", mring_create_, mring_destroy_, mring_push_, mring_pop_, 64 },
};

typedef struct {
    const queue_ops_t *ops;
    void *q;
    size_t items;           /* per producer */
    atomic_size_t remaining; /* items left to consume, shared by consumers */
    atomic_int start;
} qbench_t;

static void *qb_producer(void *p)
{
    qbench_t *b = p;
    while (!atomic_load_explicit(&b->start, memory_order_acquire))
        ck_cpu_relax();
    for (uintptr_t i = 1; i <= b->items; i++)
        while (!b->ops->push(b->q, (void *)i))
            ck_cpu_relax();
    return NULL;
}

static void *qb_consumer(void *p)
{
    qbench_t *b = p;
    while (!atomic_load_explicit(&b->start, memory_order_acquire))
        ck_cpu_relax();
    for (;;) {
        /* Reserve one item to consume, or stop if none are left. */
        size_t left = atomic_load_explicit(&b->remaining, memory_order_relaxed);
        do {
            if (left == 0)
                return NULL;
        } while (!atomic_compare_exchange_weak_explicit(&b->remaining, &left, left - 1,
                                                        memory_order_relaxed,
                                                        memory_order_relaxed));
        void *v = NULL;
        while (!b->ops->pop(b->q, &v))
            ck_cpu_relax();
    }
}

static double run_queue_bench(const queue_ops_t *ops, int producers, int consumers, size_t items)
{
    qbench_t b = { .ops = ops, .q = ops->create(1024), .items = items };
    atomic_init(&b.remaining, items * (size_t)producers);
    atomic_init(&b.start, 0);

    pthread_t th[128];
    int n = 0;
    for (int i = 0; i < producers; i++)
        pthread_create(&th[n++], NULL, qb_producer, &b);
    for (int i = 0; i < consumers; i++)
        pthread_create(&th[n++], NULL, qb_consumer, &b);

    double t0 = now_sec();
    atomic_store_explicit(&b.start, 1, memory_order_release);
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    double elapsed = now_sec() - t0;

    ops->destroy(b.q);
    return (double)(items * (size_t)producers) / elapsed / 1e6;
}

/* ---------------------------------------------------------- lock bench --- */

typedef enum { LOCK_TAS, LOCK_TICKET, LOCK_MUTEX } lock_kind_t;

typedef struct {
    lock_kind_t kind;
    ck_spinlock_t tas;
    ck_ticket_lock_t ticket;
    pthread_mutex_t mutex;
    long counter;
    long iters;
} lbench_t;

static void *lock_worker(void *p)
{
    lbench_t *b = p;
    for (long i = 0; i < b->iters; i++) {
        switch (b->kind) {
        case LOCK_TAS:
            ck_spin_lock(&b->tas);
            b->counter++;
            ck_spin_unlock(&b->tas);
            break;
        case LOCK_TICKET:
            ck_ticket_lock(&b->ticket);
            b->counter++;
            ck_ticket_unlock(&b->ticket);
            break;
        case LOCK_MUTEX:
            pthread_mutex_lock(&b->mutex);
            b->counter++;
            pthread_mutex_unlock(&b->mutex);
            break;
        }
    }
    return NULL;
}

static double run_lock_bench(lock_kind_t kind, int threads, long iters)
{
    lbench_t b = { .kind = kind, .iters = iters };
    ck_spin_init(&b.tas);
    ck_ticket_init(&b.ticket);
    pthread_mutex_init(&b.mutex, NULL);

    pthread_t th[64];
    double t0 = now_sec();
    for (int i = 0; i < threads; i++)
        pthread_create(&th[i], NULL, lock_worker, &b);
    for (int i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    double elapsed = now_sec() - t0;

    pthread_mutex_destroy(&b.mutex);
    if (b.counter != iters * threads) {
        fprintf(stderr, "lock bench: lost updates!\n");
        exit(1);
    }
    return (double)b.counter / elapsed / 1e6;
}

int main(int argc, char **argv)
{
    size_t items = argc > 1 ? strtoull(argv[1], NULL, 10) : 2000000;

    printf("Queue throughput (Mops/s, higher is better), %zu items per producer\n", items);
    printf("%-12s %8s %8s %8s %8s\n", "queue", "1P/1C", "2P/2C", "4P/4C", "8P/1C");
    for (size_t q = 0; q < sizeof(QUEUES) / sizeof(QUEUES[0]); q++) {
        const queue_ops_t *ops = &QUEUES[q];
        printf("%-12s", ops->name);
        const int configs[][2] = { { 1, 1 }, { 2, 2 }, { 4, 4 }, { 8, 1 } };
        for (int c = 0; c < 4; c++) {
            int p = configs[c][0], k = configs[c][1];
            if (p > ops->max_threads_per_side || k > ops->max_threads_per_side)
                printf(" %8s", "n/a");
            else
                printf(" %8.1f", run_queue_bench(ops, p, k, items));
            fflush(stdout);
        }
        printf("\n");
    }

    long iters = (long)items;
    printf("\nLock throughput (M acquisitions/s), %ld per thread\n", iters);
    printf("%-12s %8s %8s %8s\n", "lock", "1T", "4T", "8T");
    const char *names[] = { "tas_backoff", "ticket", "pthread" };
    for (int k = 0; k < 3; k++) {
        printf("%-12s", names[k]);
        const int tcounts[] = { 1, 4, 8 };
        for (int t = 0; t < 3; t++) {
            printf(" %8.1f", run_lock_bench((lock_kind_t)k, tcounts[t], iters));
            fflush(stdout);
        }
        printf("\n");
    }
    return 0;
}
