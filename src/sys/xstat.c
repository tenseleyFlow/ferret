#include "sys/xstat.h"
#include "config.h"

#include <fcntl.h>
#include <string.h>

enum frt_type frt_type_from_mode(mode_t m)
{
	switch (m & S_IFMT) {
	case S_IFDIR:  return FRT_DIR;
	case S_IFREG:  return FRT_REG;
	case S_IFLNK:  return FRT_LNK;
	case S_IFIFO:  return FRT_FIFO;
	case S_IFSOCK: return FRT_SOCK;
	case S_IFCHR:  return FRT_CHR;
	case S_IFBLK:  return FRT_BLK;
	default:       return FRT_UNKNOWN;
	}
}

/* Sub-second time fields are spelled st_*tim (POSIX) on Linux/FreeBSD and
 * st_*timespec on macOS/older BSD; the configure probe picks one. */
#if FRT_HAS_ST_MTIM
#define ATIM(st) ((st).st_atim)
#define MTIM(st) ((st).st_mtim)
#define CTIM(st) ((st).st_ctim)
#elif FRT_HAS_ST_MTIMESPEC
#define ATIM(st) ((st).st_atimespec)
#define MTIM(st) ((st).st_mtimespec)
#define CTIM(st) ((st).st_ctimespec)
#endif

int frt_stat_at(int dirfd, const char *name, int follow, struct frt_statinfo *o)
{
	struct stat st;
	int flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;
	if (fstatat(dirfd, name, &st, flags) < 0)
		return -1;
	memset(o, 0, sizeof *o);
	o->mode = st.st_mode;
	o->ino = st.st_ino;
	o->dev = st.st_dev;
	o->rdev = st.st_rdev;
	o->size = st.st_size;
	o->uid = st.st_uid;
	o->gid = st.st_gid;
	o->nlink = st.st_nlink;
	o->blocks = st.st_blocks;
	o->blksize = st.st_blksize;
	o->atime = st.st_atime;
	o->mtime = st.st_mtime;
	o->ctime = st.st_ctime;
#if defined(ATIM)
	o->atime_ns = (long)ATIM(st).tv_nsec;
	o->mtime_ns = (long)MTIM(st).tv_nsec;
	o->ctime_ns = (long)CTIM(st).tv_nsec;
#endif
#if FRT_HAS_ST_BIRTHTIM
	o->btime = st.st_birthtim.tv_sec;
	o->btime_ns = (long)st.st_birthtim.tv_nsec;
	o->have_btime = (st.st_birthtim.tv_nsec >= 0);
#elif FRT_HAS_ST_BIRTHTIMESPEC
	o->btime = st.st_birthtimespec.tv_sec;
	o->btime_ns = (long)st.st_birthtimespec.tv_nsec;
	o->have_btime = (st.st_birthtimespec.tv_nsec >= 0);
#endif
	return 0;
}
