#ifndef FRT_SYS_FS_H
#define FRT_SYS_FS_H

/*
 * Filesystem-type lookup for -fstype and the %F printf directive. Platform
 * abstraction: BSD/macOS read statfs.f_fstypename directly (matches find);
 * Linux maps statfs.f_type magic numbers to common names. Per overview §1,
 * exact fs-type strings are platform-specific and not strictly parity-bound.
 */

/* Returns the filesystem type name for `name` under `dirfd`, or "" on failure.
 * Does not follow symlinks (matches find's -P -fstype using the entry's own
 * device); falls back to the containing directory's fs for symlinks/broken
 * links. The returned pointer is valid until the next call (static buffer). */
const char *frt_fstype(int dirfd, const char *name);

#endif /* FRT_SYS_FS_H */
