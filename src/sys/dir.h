#ifndef FRT_SYS_DIR_H
#define FRT_SYS_DIR_H

/*
 * Directory reading abstraction — the serial read path. Backends (selected at
 * configure time): raw getdents64 (Linux), getdirentries (FreeBSD/macOS),
 * readdir(3) fallback. All return (name, type) with type taken from the dirent's
 * d_type so the common path needs no stat (audit 03).
 *
 * "." and ".." are skipped by the reader.
 */

#include <stddef.h>

enum frt_type {
	FRT_UNKNOWN = 0,
	FRT_DIR,
	FRT_REG,
	FRT_LNK,
	FRT_FIFO,
	FRT_SOCK,
	FRT_CHR,
	FRT_BLK,
	FRT_WHT, /* whiteout (union mounts) */
};

struct frt_dir;

struct frt_dirent {
	const char *name; /* valid until the next frt_dirread on this dir */
	size_t namelen;   /* strlen(name); from d_namlen where the platform has it */
	enum frt_type type;
	unsigned long long ino; /* d_ino/d_fileno, 0 if unavailable */
};

/* Open a directory. *out is set on success. Returns 0, or -1 with errno set. */
int frt_diropen(const char *path, struct frt_dir **out);
int frt_diropen_at(int parent_fd, const char *name, struct frt_dir **out);

/* Read next entry: 1 = got one, 0 = end, -1 = error (errno set). */
int frt_dirread(struct frt_dir *d, struct frt_dirent *e);

int frt_dirfd(const struct frt_dir *d);
void frt_dirclose(struct frt_dir *d);

#endif /* FRT_SYS_DIR_H */
