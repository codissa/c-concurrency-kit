#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "ck/mpmc_queue.h"
#include "ck/spsc_ring.h"
#include "test.h"

/* ---------------------------------------------------------------- SPSC --- */

static void spsc_basic(void)
{
    ck_spsc_t *q = ck_spsc_create(5); /* rounds up to 8 */
    CHECK(q != NULL);
    CHECK_EQ(q->capacity, 8);

    void *v = NULL;
    CHECK(!ck_spsc_pop(q, &v));
    for (uintptr_t i = 1; i <= 8; i++)
        CHECK(ck_spsc_push(q, (void *)i));
    CHECK(!ck_spsc_push(q, (void *)99)); /* full */
    CHECK_EQ(ck_spsc_size(q), 8);

    for (uintptr_t i = 1; i <= 8; i++) {
        CHECK(ck_spsc_pop(q, &v));
        CHECK_EQ((uintptr_t)v, i); /* FIFO */
    }
    CHECK(!ck_spsc_pop(q, &v));
    ck_spsc_destroy(q);
}

static void spsc_wraparound(void)
{
    ck_spsc_t *q = ck_spsc_create(4);
    void *v = NULL;
    for (uintptr_t i = 0; i < 1000; i++) {
        CHECK(ck_spsc_push(q, (void *)i));
        CHECK(ck_spsc_push(q, (void *)(i + 1)));
        CHECK(ck_spsc_pop(q, &v));
        CHECK_EQ((uintptr_t)v, i);
        CHECK(ck_spsc_pop(q, &v));
        CHECK_EQ((uintptr_t)v, i + 1);
    }
    ck_spsc_destroy(q);
}

#define SPSC_ITEMS (1u << 20)

static void *spsc_producer(void *arg)
{
    ck_spsc_t *q = arg;
    for (uintptr_t i = 1; i <= SPSC_ITEMS; i++)
        while (!ck_spsc_push(q, (void *)i))
            ck_cpu_relax();
    return NULL;
}

static void spsc_two_threads(void)
{
    ck_spsc_t *q = ck_spsc_create(1024);
    pthread_t prod;
    pthread_create(&prod, NULL, spsc_producer, q);

    uintptr_t expected = 1;
    int in_order = 1;
    while (expected <= SPSC_ITEMS) {
        void *v = NULL;
        if (ck_spsc_pop(q, &v)) {
            if ((uintptr_t)v != expected)
                in_order = 0;
            expected++;
        } else {
            ck_cpu_relax();
        }
    }
    pthread_join(prod, NULL);
    CHECK(in_order);
    ck_spsc_destroy(q);
}

/* ---------------------------------------------------------------- MPMC --- */

static void mpmc_basic(void)
{
    ck_mpmc_t *q = ck_mpmc_create(4);
    void *v = NULL;
    CHECK(!ck_mpmc_pop(q, &v));
    for (uintptr_t i = 1; i <= 4; i++)
        CHECK(ck_mpmc_push(q, (void *)i));
    CHECK(!ck_mpmc_push(q, (void *)5));
    for (uintptr_t i = 1; i <= 4; i++) {
        CHECK(ck_mpmc_pop(q, &v));
        CHECK_EQ((uintptr_t)v, i);
    }
    CHECK(!ck_mpmc_pop(q, &v));
    ck_mpmc_destroy(q);
}

#define MPMC_PRODUCERS 4
#define MPMC_CONSUMERS 4
#define MPMC_PER_PRODUCER 100000u
#define MPMC_TOTAL (MPMC_PRODUCERS * MPMC_PER_PRODUCER)

struct mpmc_ctx {
    ck_mpmc_t *q;
    atomic_uint consumed;
    atomic_uchar seen[MPMC_TOTAL];
    atomic_int duplicates;
    /* Per-(consumer, producer) last sequence number: each consumer must see
     * any single producer's items in increasing order. */
    long last[MPMC_CONSUMERS][MPMC_PRODUCERS];
    atomic_int reordered;
};

struct worker_arg {
    struct mpmc_ctx *ctx;
    int id;
};

static void *mpmc_producer(void *p)
{
    struct worker_arg *a = p;
    for (uintptr_t s = 0; s < MPMC_PER_PRODUCER; s++) {
        /* +1 so that we never enqueue NULL */
        uintptr_t item = (uintptr_t)a->id * MPMC_PER_PRODUCER + s + 1;
        while (!ck_mpmc_push(a->ctx->q, (void *)item))
            ck_cpu_relax();
    }
    return NULL;
}

static void *mpmc_consumer(void *p)
{
    struct worker_arg *a = p;
    struct mpmc_ctx *ctx = a->ctx;
    while (atomic_load(&ctx->consumed) < MPMC_TOTAL) {
        void *v = NULL;
        if (!ck_mpmc_pop(ctx->q, &v)) {
            ck_cpu_relax();
            continue;
        }
        uintptr_t item = (uintptr_t)v - 1;
        if (atomic_exchange(&ctx->seen[item], 1))
            atomic_fetch_add(&ctx->duplicates, 1);

        int producer = (int)(item / MPMC_PER_PRODUCER);
        long seq = (long)(item % MPMC_PER_PRODUCER);
        if (seq <= ctx->last[a->id][producer])
            atomic_fetch_add(&ctx->reordered, 1);
        ctx->last[a->id][producer] = seq;

        atomic_fetch_add(&ctx->consumed, 1);
    }
    return NULL;
}

static void mpmc_stress(void)
{
    static struct mpmc_ctx ctx; /* large: keep off the stack */
    memset(&ctx, 0, sizeof(ctx));
    for (int c = 0; c < MPMC_CONSUMERS; c++)
        for (int p = 0; p < MPMC_PRODUCERS; p++)
            ctx.last[c][p] = -1;
    ctx.q = ck_mpmc_create(256);

    pthread_t prod[MPMC_PRODUCERS], cons[MPMC_CONSUMERS];
    struct worker_arg pa[MPMC_PRODUCERS], ca[MPMC_CONSUMERS];
    for (int i = 0; i < MPMC_CONSUMERS; i++) {
        ca[i] = (struct worker_arg){ &ctx, i };
        pthread_create(&cons[i], NULL, mpmc_consumer, &ca[i]);
    }
    for (int i = 0; i < MPMC_PRODUCERS; i++) {
        pa[i] = (struct worker_arg){ &ctx, i };
        pthread_create(&prod[i], NULL, mpmc_producer, &pa[i]);
    }
    for (int i = 0; i < MPMC_PRODUCERS; i++)
        pthread_join(prod[i], NULL);
    for (int i = 0; i < MPMC_CONSUMERS; i++)
        pthread_join(cons[i], NULL);

    CHECK_EQ(atomic_load(&ctx.consumed), MPMC_TOTAL);
    CHECK_EQ(atomic_load(&ctx.duplicates), 0);
    CHECK_EQ(atomic_load(&ctx.reordered), 0);
    unsigned missing = 0;
    for (unsigned i = 0; i < MPMC_TOTAL; i++)
        missing += !atomic_load(&ctx.seen[i]);
    CHECK_EQ(missing, 0);
    ck_mpmc_destroy(ctx.q);
}

int main(void)
{
    RUN(spsc_basic);
    RUN(spsc_wraparound);
    RUN(spsc_two_threads);
    RUN(mpmc_basic);
    RUN(mpmc_stress);
    TEST_EXIT();
}
