#include "ck/thread_pool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct ck_task {
    ck_task_fn fn;
    void *arg;
    struct ck_task *next;
} ck_task_t;

struct ck_pool {
    pthread_mutex_t lock;
    pthread_cond_t has_work; /* signalled when a task is queued or on shutdown */
    pthread_cond_t all_done; /* signalled when `pending` drops to zero         */

    ck_task_t *head;
    ck_task_t *tail;
    size_t pending; /* queued + currently running */
    bool stopping;

    size_t nthreads;
    pthread_t *threads;
};

static void *worker_main(void *p)
{
    ck_pool_t *pool = p;

    pthread_mutex_lock(&pool->lock);
    for (;;) {
        while (!pool->head && !pool->stopping)
            pthread_cond_wait(&pool->has_work, &pool->lock);

        if (!pool->head) /* stopping and nothing left to do */
            break;

        ck_task_t *task = pool->head;
        pool->head = task->next;
        if (!pool->head)
            pool->tail = NULL;

        pthread_mutex_unlock(&pool->lock);
        task->fn(task->arg);
        free(task);
        pthread_mutex_lock(&pool->lock);

        if (--pool->pending == 0)
            pthread_cond_broadcast(&pool->all_done);
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

ck_pool_t *ck_pool_create(size_t nthreads)
{
    if (nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = n > 0 ? (size_t)n : 1;
    }

    ck_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;
    pool->threads = calloc(nthreads, sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->has_work, NULL);
    pthread_cond_init(&pool->all_done, NULL);

    for (size_t i = 0; i < nthreads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0) {
            /* Shut down the workers we did manage to start. */
            pool->nthreads = i;
            ck_pool_destroy(pool);
            return NULL;
        }
    }
    pool->nthreads = nthreads;
    return pool;
}

int ck_pool_submit(ck_pool_t *pool, ck_task_fn fn, void *arg)
{
    ck_task_t *task = malloc(sizeof(*task));
    if (!task)
        return -1;
    task->fn = fn;
    task->arg = arg;
    task->next = NULL;

    pthread_mutex_lock(&pool->lock);
    if (pool->stopping) {
        pthread_mutex_unlock(&pool->lock);
        free(task);
        return -1;
    }
    if (pool->tail)
        pool->tail->next = task;
    else
        pool->head = task;
    pool->tail = task;
    pool->pending++;
    pthread_cond_signal(&pool->has_work);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void ck_pool_wait(ck_pool_t *pool)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->pending > 0)
        pthread_cond_wait(&pool->all_done, &pool->lock);
    pthread_mutex_unlock(&pool->lock);
}

void ck_pool_destroy(ck_pool_t *pool)
{
    if (!pool)
        return;

    ck_pool_wait(pool);

    pthread_mutex_lock(&pool->lock);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->has_work);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->nthreads; i++)
        pthread_join(pool->threads[i], NULL);

    pthread_cond_destroy(&pool->all_done);
    pthread_cond_destroy(&pool->has_work);
    pthread_mutex_destroy(&pool->lock);
    free(pool->threads);
    free(pool);
}

size_t ck_pool_size(const ck_pool_t *pool)
{
    return pool->nthreads;
}
