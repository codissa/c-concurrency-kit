#ifndef CK_THREAD_POOL_H
#define CK_THREAD_POOL_H

#include <stddef.h>

/*
 * Fixed-size pthread worker pool with a FIFO task queue.
 *
 * Tasks may submit further tasks. ck_pool_wait() blocks until every task
 * submitted so far (including tasks spawned by those tasks) has finished.
 */
typedef void (*ck_task_fn)(void *arg);

typedef struct ck_pool ck_pool_t;

/* nthreads == 0 means "one per online CPU". Returns NULL on failure. */
ck_pool_t *ck_pool_create(size_t nthreads);

/* Returns 0 on success, -1 if the pool is shutting down or allocation failed. */
int ck_pool_submit(ck_pool_t *pool, ck_task_fn fn, void *arg);

/* Block until the queue is empty and no task is running. */
void ck_pool_wait(ck_pool_t *pool);

/* Finish all outstanding work, join the workers and free the pool. */
void ck_pool_destroy(ck_pool_t *pool);

size_t ck_pool_size(const ck_pool_t *pool);

#endif /* CK_THREAD_POOL_H */
