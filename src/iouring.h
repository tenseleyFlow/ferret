#ifndef FRT_IOURING_H
#define FRT_IOURING_H

#include <stddef.h>
#include <stdint.h>

/*
 * Optional Linux io_uring statx backend for the parallel stat pass. Same role as
 * the worker pool (src/pool.c): given a directory's surviving entries, fill their
 * metadata. It batches statx requests through one ring instead of fanning fstatat
 * across threads. Output is byte-identical to the fstatat path — the fill mirrors
 * frt_stat_at exactly (no STATX_BTIME, since fstatat on Linux yields no birthtime).
 *
 * frt_iouring_create returns NULL when io_uring/statx is unavailable (old kernel,
 * seccomp, no liburing at build time); the caller falls back to the worker pool.
 * Single-thread use only (the eval thread), like the rest of the stat path.
 */

struct entry;          /* entry.h */
struct frt_statinfo;   /* sys/xstat.h */
struct frt_iouring;

/* Create a ring (qd = suggested queue depth). NULL if io_uring is unavailable. */
struct frt_iouring *frt_iouring_create(unsigned qd);
void frt_iouring_destroy(struct frt_iouring *r);

/* statx the survivors (surv[k] indexes ents/, k in [0,ns)) into slots[k], applying
 * the result to each entry just as frt_entry_fill_stat does for fstatat. follow!=0
 * follows symlinks. Entries whose statx fails get ENT_STAT_FAILED + errno. */
void frt_iouring_statx_batch(struct frt_iouring *r, int dirfd, struct entry **ents,
			     const uint32_t *surv, size_t ns, int follow,
			     struct frt_statinfo *slots);

#endif /* FRT_IOURING_H */
