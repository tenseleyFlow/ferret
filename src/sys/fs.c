#include "sys/fs.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/vfs.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif

#if defined(__linux__)
static const char *type_name(const struct statfs *sfs, char *buf, size_t n)
{
	/* Fallback only: statfs magic cannot tell ext2/3/4 apart (all 0xEF53) and
	 * GNU find names them from the mount table, so the mountinfo lookup below is
	 * tried first. This map covers files with no matching mount entry. */
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

/* device -> fs-type name, parsed once from /proc/self/mountinfo. GNU find names
 * %F / -fstype from the mount table (gnulib mountlist), so "ext4", "overlay",
 * "tmpfs" etc. come out verbatim rather than a statfs-magic approximation. */
struct mnt_dev {
	dev_t dev;
	char type[32];
};
static struct mnt_dev mnt_tab[512];
static size_t mnt_n;
static int mnt_loaded;

static void load_mountinfo(void)
{
	mnt_loaded = 1;
	FILE *f = fopen("/proc/self/mountinfo", "re");
	if (!f)
		return;
	char line[8192];
	while (mnt_n < sizeof mnt_tab / sizeof mnt_tab[0] && fgets(line, sizeof line, f)) {
		/* fields: id parent major:minor root mp opts [optional...] - fstype ... */
		unsigned ma = 0, mi = 0;
		int have_dev = 0, idx = 0;
		const char *fstype = NULL;
		char *save = NULL;
		for (char *tok = strtok_r(line, " ", &save); tok;
		     tok = strtok_r(NULL, " ", &save), idx++) {
			if (idx == 2)
				have_dev = (sscanf(tok, "%u:%u", &ma, &mi) == 2);
			else if (idx > 2 && strcmp(tok, "-") == 0) {
				fstype = strtok_r(NULL, " ", &save);
				break;
			}
		}
		if (have_dev && fstype) {
			mnt_tab[mnt_n].dev = makedev(ma, mi);
			snprintf(mnt_tab[mnt_n].type, sizeof mnt_tab[mnt_n].type, "%s", fstype);
			mnt_n++;
		}
	}
	fclose(f);
}

static const char *mountinfo_type(dev_t dev)
{
	if (!mnt_loaded)
		load_mountinfo();
	for (size_t i = 0; i < mnt_n; i++)
		if (mnt_tab[i].dev == dev)
			return mnt_tab[i].type;
	return NULL;
}
#endif

const char *frt_fstype(int dirfd, const char *name)
{
	static char buf[64];
	struct statfs sfs;
	int have = 0;
#if defined(__linux__)
	dev_t dev = 0;
	int have_dev = 0;
#endif

	/* Don't follow symlinks: fstatfs the entry itself; for symlinks (incl.
	 * broken) the open fails, so fall back to the containing directory's fs —
	 * which is where the link lives, matching find's -P behavior. O_NONBLOCK so
	 * a FIFO (O_RDONLY blocks until a writer appears) returns at once instead of
	 * wedging the whole traversal; harmless for dirs and regular files. */
	int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		have = (fstatfs(fd, &sfs) == 0);
#if defined(__linux__)
		struct stat st;
		if (fstat(fd, &st) == 0) {
			dev = st.st_dev;
			have_dev = 1;
		}
#endif
		close(fd);
	}
	if (!have)
		have = (fstatfs(dirfd, &sfs) == 0);
#if defined(__linux__)
	if (!have_dev) {
		struct stat st;
		if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
		    fstat(dirfd, &st) == 0) {
			dev = st.st_dev;
			have_dev = 1;
		}
	}
#endif
	if (!have)
		return "";

#if defined(__linux__)
	if (have_dev) {
		const char *t = mountinfo_type(dev);
		if (t)
			return t; /* find's mount-table name (ext4, overlay, ...) */
	}
	return type_name(&sfs, buf, sizeof buf); /* fallback: statfs magic */
#else
	snprintf(buf, sizeof buf, "%s", sfs.f_fstypename);
	return buf;
#endif
}
