#ifndef FRT_POOL_H
#define FRT_POOL_H

/*
 * Minimal persistent worker pool for the parallel stat pass (sprint 10). The
 * only thing ferret parallelizes is per-entry fstatat() on a directory's
 * entries; output order is decided before any stat runs and concurrent fstatat
 * on a shared dir fd is stateless, so this changes timing only, never bytes.
 * Workers touch disjoint entries; no locking on the hot path. The pthread
 * runtime is dlopen'd lazily so a serial run pays nothing (ported from aspen).
 */

#include <stddef.h>

struct frt_pool;

/* Create a pool of `workers` threads (the submitting thread is one lane).
 * Returns NULL if workers <= 1 or the thread runtime is unavailable — callers
 * treat NULL as serial. */
struct frt_pool *frt_pool_create(int workers);
void frt_pool_destroy(struct frt_pool *p);

/* Run fn(arg, i) for i in [0, n). Blocks until all iterations finish. With a
 * NULL pool it runs inline. */
void frt_pool_for(struct frt_pool *p, size_t n, void (*fn)(void *, size_t), void *arg);

/* Online CPU count, capped (stat work is kernel-bound). */
int frt_pool_default_workers(void);

#endif /* FRT_POOL_H */
