#ifndef FRT_SYS_XSTAT_H
#define FRT_SYS_XSTAT_H

/*
 * stat abstraction — the metadata path. Only called when a predicate needs
 * metadata or d_type was UNKNOWN (audit 03). frt_stat_at is the fstatat
 * baseline; the io_uring backend (iouring.c, sprint 10) issues statx batches.
 */

#include "sys/dir.h" /* enum frt_type */

#include <sys/types.h>
#include <sys/stat.h>

/* THE sole metadata contract. Every predicate/format directive that needs file
 * metadata reads ONLY these fields — nothing reaches behind to a raw struct stat.
 * Both stat backends must populate ALL of them for an entry they succeed on:
 *   - frt_stat_at()  (fstatat baseline, xstat.c)
 *   - the io_uring statx batch (iouring.c) — its STATX request mask must cover
 *     every field below, or a predicate sees a stale/zero value on that backend
 *     only (an invisible, backend-specific divergence).
 * Wider than aspen's: find needs atime, nlink, blocks, blksize, rdev, sub-second
 * times, and birthtime (audit 02). Add a field here => extend BOTH backends. */
struct frt_statinfo {
	mode_t mode;
	ino_t ino;
	dev_t dev;
	dev_t rdev;
	off_t size;
	uid_t uid;
	gid_t gid;
	nlink_t nlink;
	blkcnt_t blocks;
	blksize_t blksize;
	time_t atime, mtime, ctime, btime;
	long atime_ns, mtime_ns, ctime_ns, btime_ns;
	int have_btime; /* birthtime is valid (platform + fs provide it) */
};

enum frt_type frt_type_from_mode(mode_t m);

/* fstatat relative to dirfd; follow=0 => AT_SYMLINK_NOFOLLOW. 0 ok, -1 errno. */
int frt_stat_at(int dirfd, const char *name, int follow, struct frt_statinfo *o);

#endif /* FRT_SYS_XSTAT_H */
