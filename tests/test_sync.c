#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "ck/slab.h"
#include "ck/spinlock.h"
#include "ck/thread_pool.h"
#include "test.h"

#define THREADS 8
#define ITERS 50000

/* ------------------------------------------------------------ spinlocks --- */

static ck_spinlock_t tas_lock = CK_SPINLOCK_INIT;
static ck_ticket_lock_t ticket_lock = CK_TICKET_LOCK_INIT;
static long shared_counter;

static void *tas_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        ck_spin_lock(&tas_lock);
        shared_counter++; /* non-atomic on purpose: the lock must protect it */
        ck_spin_unlock(&tas_lock);
    }
    return NULL;
}

static void *ticket_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        ck_ticket_lock(&ticket_lock);
        shared_counter++;
        ck_ticket_unlock(&ticket_lock);
    }
    return NULL;
}

static void run_threads(void *(*fn)(void *), void *arg)
{
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, fn, arg);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
}

static void spinlock_mutual_exclusion(void)
{
    shared_counter = 0;
    run_threads(tas_worker, NULL);
    CHECK_EQ(shared_counter, (long)THREADS * ITERS);
}

static void ticket_lock_mutual_exclusion(void)
{
    shared_counter = 0;
    run_threads(ticket_worker, NULL);
    CHECK_EQ(shared_counter, (long)THREADS * ITERS);
}

static void spinlock_trylock(void)
{
    ck_spinlock_t l;
    ck_spin_init(&l);
    CHECK(ck_spin_trylock(&l));
    CHECK(!ck_spin_trylock(&l));
    ck_spin_unlock(&l);
    CHECK(ck_spin_trylock(&l));
    ck_spin_unlock(&l);
}

/* ---------------------------------------------------------- thread pool --- */

static atomic_long pool_sum;

static void add_task(void *arg)
{
    atomic_fetch_add(&pool_sum, (long)(intptr_t)arg);
}

static void pool_many_tasks(void)
{
    ck_pool_t *pool = ck_pool_create(4);
    CHECK(pool != NULL);
    CHECK_EQ(ck_pool_size(pool), 4);

    atomic_store(&pool_sum, 0);
    const long n = 20000;
    for (long i = 1; i <= n; i++)
        CHECK_EQ(ck_pool_submit(pool, add_task, (void *)(intptr_t)i), 0);
    ck_pool_wait(pool);
    CHECK_EQ(atomic_load(&pool_sum), n * (n + 1) / 2);

    /* The pool is reusable after wait(). */
    ck_pool_submit(pool, add_task, (void *)(intptr_t)1);
    ck_pool_wait(pool);
    CHECK_EQ(atomic_load(&pool_sum), n * (n + 1) / 2 + 1);
    ck_pool_destroy(pool);
}

/* Tasks that spawn subtasks: parallel recursive tree sum. */
struct tree_arg {
    ck_pool_t *pool;
    int depth;
};

static void tree_task(void *p)
{
    struct tree_arg *a = p;
    atomic_fetch_add(&pool_sum, 1);
    if (a->depth > 0) {
        for (int i = 0; i < 2; i++) {
            struct tree_arg *child = malloc(sizeof(*child));
            child->pool = a->pool;
            child->depth = a->depth - 1;
            ck_pool_submit(a->pool, tree_task, child);
        }
    }
    free(a);
}

static void pool_nested_submit(void)
{
    ck_pool_t *pool = ck_pool_create(0);
    atomic_store(&pool_sum, 0);

    struct tree_arg *root = malloc(sizeof(*root));
    root->pool = pool;
    root->depth = 12;
    ck_pool_submit(pool, tree_task, root);
    ck_pool_wait(pool); /* must also wait for grandchildren */

    CHECK_EQ(atomic_load(&pool_sum), (1L << 13) - 1);
    ck_pool_destroy(pool);
}

static void pool_destroy_drains(void)
{
    ck_pool_t *pool = ck_pool_create(2);
    atomic_store(&pool_sum, 0);
    for (int i = 0; i < 1000; i++)
        ck_pool_submit(pool, add_task, (void *)(intptr_t)1);
    ck_pool_destroy(pool); /* no explicit wait */
    CHECK_EQ(atomic_load(&pool_sum), 1000);
}

/* ----------------------------------------------------------------- slab --- */

struct obj {
    uint64_t owner;
    uint64_t payload[7];
};

static void slab_basic(void)
{
    ck_slab_t *s = ck_slab_create(sizeof(struct obj), 3);
    void *a = ck_slab_alloc(s), *b = ck_slab_alloc(s), *c = ck_slab_alloc(s);
    CHECK(a && b && c);
    CHECK(a != b && b != c && a != c);
    CHECK(ck_slab_alloc(s) == NULL); /* exhausted */
    CHECK_EQ(ck_slab_in_use(s), 3);

    ck_slab_free(s, b);
    CHECK(ck_slab_alloc(s) == b); /* LIFO reuse */
    ck_slab_free(s, a);
    ck_slab_free(s, b);
    ck_slab_free(s, c);
    CHECK_EQ(ck_slab_in_use(s), 0);
    CHECK(ck_slab_create(0, 10) == NULL);
    ck_slab_destroy(s);
}

static ck_slab_t *stress_slab;
static atomic_int slab_corruptions;

static void *slab_worker(void *arg)
{
    uint64_t me = (uint64_t)(intptr_t)arg + 1;
    struct obj *held[16];

    for (int round = 0; round < 5000; round++) {
        int n = 0;
        for (; n < 16; n++) {
            held[n] = ck_slab_alloc(stress_slab);
            if (!held[n])
                break;
            /* Stamp the object; if two threads ever get the same object the
             * stamp check below will catch it. */
            held[n]->owner = me;
            for (int k = 0; k < 7; k++)
                held[n]->payload[k] = me * 1000 + (uint64_t)k;
        }
        for (int i = 0; i < n; i++) {
            if (held[i]->owner != me || held[i]->payload[6] != me * 1000 + 6)
                atomic_fetch_add(&slab_corruptions, 1);
            ck_slab_free(stress_slab, held[i]);
        }
    }
    return NULL;
}

static void slab_concurrent(void)
{
    /* Fewer objects than THREADS*16 so threads regularly hit exhaustion. */
    stress_slab = ck_slab_create(sizeof(struct obj), 64);
    atomic_store(&slab_corruptions, 0);

    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, slab_worker, (void *)(intptr_t)i);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);

    CHECK_EQ(atomic_load(&slab_corruptions), 0);
    CHECK_EQ(ck_slab_in_use(stress_slab), 0);

    /* Every object must be back on the free list exactly once. */
    void *seen[64];
    int count = 0;
    void *p;
    while ((p = ck_slab_alloc(stress_slab)) != NULL) {
        for (int i = 0; i < count; i++)
            CHECK(seen[i] != p);
        if (count < 64)
            seen[count] = p;
        count++;
    }
    CHECK_EQ(count, 64);
    ck_slab_destroy(stress_slab);
}

int main(void)
{
    RUN(spinlock_mutual_exclusion);
    RUN(ticket_lock_mutual_exclusion);
    RUN(spinlock_trylock);
    RUN(pool_many_tasks);
    RUN(pool_nested_submit);
    RUN(pool_destroy_drains);
    RUN(slab_basic);
    RUN(slab_concurrent);
    TEST_EXIT();
}
