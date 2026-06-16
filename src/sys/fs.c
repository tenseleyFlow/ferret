#include "sys/fs.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif

#if defined(__linux__)
static const char *type_name(const struct statfs *sfs, char *buf, size_t n)
{
	/* Minimal magic -> name map (common types). find ships a fuller table;
	 * exact Linux fs names are not parity-bound (overview §1). */
	switch ((unsigned long)sfs->f_type) {
	case 0xEF53:     return "ext2/ext3";
	case 0x58465342: return "xfs";
	case 0x9123683E: return "btrfs";
	case 0x01021994: return "tmpfs";
	case 0x6969:     return "nfs";
	case 0xFF534D42: return "cifs";
	case 0x4d44:     return "msdos";
	case 0x2FC12FC1: return "zfs";
	case 0x794c7630: return "overlayfs";
	case 0x9fa0:     return "proc";
	case 0x62656572: return "sysfs";
	case 0x1cd1:     return "devpts";
	default:
		snprintf(buf, n, "0x%lx", (unsigned long)sfs->f_type);
		return buf;
	}
}
#endif

const char *frt_fstype(int dirfd, const char *name)
{
	static char buf[64];
	struct statfs sfs;
	int have = 0;

	/* Don't follow symlinks: fstatfs the entry itself; for symlinks (incl.
	 * broken) the open fails, so fall back to the containing directory's fs —
	 * which is where the link lives, matching find's -P behavior. O_NONBLOCK so
	 * a FIFO (O_RDONLY blocks until a writer appears) returns at once instead of
	 * wedging the whole traversal; harmless for dirs and regular files. */
	int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		have = (fstatfs(fd, &sfs) == 0);
		close(fd);
	}
	if (!have)
		have = (fstatfs(dirfd, &sfs) == 0);
	if (!have)
		return "";

#if defined(__linux__)
	return type_name(&sfs, buf, sizeof buf);
#else
	snprintf(buf, sizeof buf, "%s", sfs.f_fstypename);
	return buf;
#endif
}
