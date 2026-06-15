#ifndef FRT_ENTRY_H
#define FRT_ENTRY_H

/*
 * One directory entry. Arena-allocated with the name inline (one allocation per
 * entry, cache-friendly — overview §8). Metadata is filled lazily only when a
 * predicate forces a stat; the common path leaves `st` NULL.
 *
 * Unlike aspen's entry, ferret streams: no in-memory child tree, no render
 * state. The walker evaluates each entry against the expression and moves on.
 */

#include "arena.h"
#include "sys/dir.h"   /* enum frt_type */
#include "sys/xstat.h" /* struct frt_statinfo */

#include <stdint.h>
#include <sys/types.h>

enum {
	ENT_STATTED     = 1u << 0, /* st is populated */
	ENT_STAT_FAILED = 1u << 1, /* a forced stat failed (errno saved in stat_errno) */
	ENT_ORPHAN      = 1u << 2, /* dangling symlink (target stat failed under -L/-xtype) */
	ENT_HAVE_LNK    = 1u << 3, /* lnk is populated (readlink done) */
};

struct entry {
	uint32_t namelen;
	uint16_t type;  /* enum frt_type (from d_type or lstat) */
	uint16_t ltype; /* symlink target type when followed, else FRT_UNKNOWN */
	uint16_t flags;
	uint16_t _pad;
	int depth;             /* depth below the start path (start path = 0) */
	int stat_errno;        /* errno from a failed forced stat */
	ino_t ino;             /* from d_ino or stat */
	dev_t dev;             /* owning device (filled on stat; used by -xdev) */
	const struct frt_statinfo *st; /* lazy metadata, NULL until a predicate forces it */
	char *lnk;             /* symlink target string, or NULL */
	const char *path;      /* full path as walked (into the shared path buffer) */
	uint32_t pathlen;      /* length of path */
	uint32_t basepos;      /* offset of the basename within path (for %f/%h) */
	char name[];           /* inline, NUL-terminated basename */
};

struct entry *entry_new(struct arena *a, const char *name, size_t namelen,
			enum frt_type type);

#endif /* FRT_ENTRY_H */
