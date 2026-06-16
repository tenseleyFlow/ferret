#include "config.h"
#include "iouring.h"

#if FRT_HAS_LIBURING

#include "entry.h"
#include "sys/xstat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <liburing.h>

#define FRT_URING_QD 256u

struct frt_iouring {
	struct io_uring ring;
	struct statx bufs[FRT_URING_QD];
};

/* Map statx -> frt_statinfo identically to frt_stat_at's fstatat fill. Deliberately
 * NO birthtime: fstatat on Linux returns none, so have_btime stays 0 and the two
 * backends produce byte-identical output (the design's load-bearing invariant). */
static void statx_to_statinfo(const struct statx *sx, struct frt_statinfo *o)
{
	memset(o, 0, sizeof *o);
	o->mode = sx->stx_mode;
	o->ino = sx->stx_ino;
	o->dev = makedev(sx->stx_dev_major, sx->stx_dev_minor);
	o->rdev = makedev(sx->stx_rdev_major, sx->stx_rdev_minor);
	o->size = (off_t)sx->stx_size;
	o->uid = sx->stx_uid;
	o->gid = sx->stx_gid;
	o->nlink = sx->stx_nlink;
	o->blocks = (blkcnt_t)sx->stx_blocks;
	o->blksize = (blksize_t)sx->stx_blksize;
	o->atime = (time_t)sx->stx_atime.tv_sec;
	o->mtime = (time_t)sx->stx_mtime.tv_sec;
	o->ctime = (time_t)sx->stx_ctime.tv_sec;
	o->atime_ns = (long)sx->stx_atime.tv_nsec;
	o->mtime_ns = (long)sx->stx_mtime.tv_nsec;
	o->ctime_ns = (long)sx->stx_ctime.tv_nsec;
}

struct frt_iouring *frt_iouring_create(unsigned qd)
{
	(void)qd;
	struct frt_iouring *r = malloc(sizeof *r);
	if (!r)
		return NULL;
	if (io_uring_queue_init(FRT_URING_QD, &r->ring, 0) < 0) {
		free(r); /* old kernel / seccomp / sandbox -> caller uses the pool */
		return NULL;
	}
	return r;
}

void frt_iouring_destroy(struct frt_iouring *r)
{
	if (!r)
		return;
	io_uring_queue_exit(&r->ring);
	free(r);
}

static void apply(struct entry *ent, struct frt_statinfo *slot, int follow)
{
	ent->st = slot;
	ent->flags |= ENT_STATTED;
	if (follow || ent->type == FRT_UNKNOWN)
		ent->type = (uint16_t)frt_type_from_mode(slot->mode);
	ent->ino = slot->ino;
	ent->dev = slot->dev;
}

void frt_iouring_statx_batch(struct frt_iouring *r, int dirfd, struct entry **ents,
			     const uint32_t *surv, size_t ns, int follow,
			     struct frt_statinfo *slots)
{
	int flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;
	size_t base = 0;
	while (base < ns) {
		size_t chunk = ns - base;
		if (chunk > FRT_URING_QD)
			chunk = FRT_URING_QD;

		size_t prepped = 0;
		for (; prepped < chunk; prepped++) {
			struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
			if (!sqe)
				break;
			const char *nm = ents[surv[base + prepped]]->name;
			io_uring_prep_statx(sqe, dirfd, nm, flags, STATX_BASIC_STATS,
					    &r->bufs[prepped]);
			io_uring_sqe_set_data(sqe, (void *)(uintptr_t)prepped);
		}
		if (prepped == 0)
			break; /* can't make progress; eval_expr lazy-stats the rest */

		int sub;
		do {
			sub = io_uring_submit(&r->ring);
		} while (sub == -EINTR);
		if (sub <= 0)
			break; /* submit failed; eval_expr lazy-stats the rest, ring left clean */
		size_t want = (size_t)sub < prepped ? (size_t)sub : prepped;

		/* Reap exactly the completions we asked for. wait only `want` times so a
		 * short submit can't block forever; advance base by what we actually
		 * reaped so a wait failure can't skip un-stat'd entries (they fall back to
		 * eval_expr's lazy fstatat). */
		size_t done = 0;
		for (; done < want; done++) {
			struct io_uring_cqe *cqe;
			int w;
			while ((w = io_uring_wait_cqe(&r->ring, &cqe)) == -EINTR)
				;
			if (w < 0)
				break;
			size_t j = (size_t)(uintptr_t)io_uring_cqe_get_data(cqe);
			size_t k = base + j;
			struct entry *ent = ents[surv[k]];
			if (cqe->res == 0) {
				statx_to_statinfo(&r->bufs[j], &slots[k]);
				apply(ent, &slots[k], follow);
			} else {
				ent->flags |= ENT_STAT_FAILED;
				ent->stat_errno = -cqe->res;
			}
			io_uring_cqe_seen(&r->ring, cqe);
		}
		base += done;
		if (done < prepped)
			break; /* didn't fully drain this chunk; lazy-stat the remainder */
	}
}

#else /* no liburing at build time: stubs; create() returns NULL so callers fall back */

struct frt_iouring *frt_iouring_create(unsigned qd)
{
	(void)qd;
	return NULL;
}
void frt_iouring_destroy(struct frt_iouring *r) { (void)r; }
void frt_iouring_statx_batch(struct frt_iouring *r, int dirfd, struct entry **ents,
			     const uint32_t *surv, size_t ns, int follow,
			     struct frt_statinfo *slots)
{
	(void)r; (void)dirfd; (void)ents; (void)surv; (void)ns; (void)follow; (void)slots;
}

#endif
