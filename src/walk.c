#include "walk.h"
#include "eval.h"
#include "entry.h"
#include "util.h"
#include "diag.h"
#include "exec.h"
#include "sys/dir.h"
#include "sys/xstat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ancestor {
	dev_t dev;
	ino_t ino;
	char *path; /* path of this ancestor dir, for the loop diagnostic */
};

struct walkenv {
	const struct options *opts;
	const struct expr *expr;
	struct arena arena;
	struct dstr path;
	struct evalctx ctx;
	dev_t start_dev;   /* -xdev: device of the start path */
	struct ancestor *anc; /* -L: (dev,ino) of directories on the current path */
	int nanc, anccap;
	int dir_seq;       /* monotonic directory-instance counter (-execdir +) */
};

static void report_error(struct walkenv *we, const char *path, int err)
{
	frt_diag_errno("", path, err);
	*we->ctx.exit_status = 1;
}

static void report_loop(struct walkenv *we, const char *child, const char *ancestor)
{
	const char *q1 = frt_diag_utf8() ? "\xe2\x80\x98" : "'";
	const char *q2 = frt_diag_utf8() ? "\xe2\x80\x99" : "'";
	fprintf(stderr,
		"ferret: File system loop detected; %s%s%s is part of the same file system "
		"loop as %s%s%s.\n",
		q1, child, q2, q1, ancestor, q2);
	*we->ctx.exit_status = 1;
}

/* basename of a start path: strip trailing slashes (keep one if all slashes),
 * then the last component. "a/b/" -> "b", "/" -> "/", "." -> ".". */
static const char *base_of(const char *path, size_t *len)
{
	size_t n = strlen(path);
	while (n > 1 && path[n - 1] == '/')
		n--;
	size_t start = 0;
	for (size_t k = 0; k < n; k++)
		if (path[k] == '/')
			start = k + 1;
	*len = n - start;
	return path + start;
}

/* Whether to treat `ent` as a directory for descent. Under -L, follow symlinks
 * (the target's type decides); otherwise d_type/lstat. */
static int entry_is_dir(struct walkenv *we, struct entry *ent)
{
	if (ent->type == FRT_DIR)
		return 1;
	if (we->ctx.follow == 1) {
		/* -L: a symlink to a directory is descended into (following it). */
		if (ent->type == FRT_LNK || ent->type == FRT_UNKNOWN) {
			const struct frt_statinfo *st = entry_stat(ent, &we->ctx);
			return st && frt_type_from_mode(st->mode) == FRT_DIR;
		}
		return 0;
	}
	if (ent->type == FRT_UNKNOWN) {
		entry_stat(ent, &we->ctx);
		return ent->type == FRT_DIR;
	}
	return 0;
}

/* May we descend into directory `ent` (sitting at `depth`)? Enforces -maxdepth,
 * -xdev, and -L loop detection. */
static int may_descend(struct walkenv *we, struct entry *ent, int depth)
{
	const struct options *o = we->opts;
	if (o->maxdepth >= 0 && depth >= o->maxdepth)
		return 0;
	if (o->xdev) {
		const struct frt_statinfo *st = entry_stat(ent, &we->ctx);
		if (!st || st->dev != we->start_dev)
			return 0;
	}
	return 1;
}

/* Under -L, a directory whose (dev,ino) matches an ancestor on the current path
 * is a loop: find emits a diagnostic and skips the entry entirely (no eval, no
 * descent). Returns the ancestor path if a loop, else NULL. */
static const char *loop_ancestor(struct walkenv *we, struct entry *ent)
{
	if (we->ctx.follow != 1)
		return NULL;
	const struct frt_statinfo *st = entry_stat(ent, &we->ctx);
	if (!st)
		return NULL;
	for (int k = 0; k < we->nanc; k++)
		if (we->anc[k].dev == st->dev && we->anc[k].ino == st->ino)
			return we->anc[k].path;
	return NULL;
}

static void anc_push(struct walkenv *we, dev_t dev, ino_t ino, const char *path)
{
	if (we->nanc == we->anccap) {
		we->anccap = we->anccap ? we->anccap * 2 : 16;
		we->anc = frt_xrealloc(we->anc, (size_t)we->anccap * sizeof *we->anc);
	}
	we->anc[we->nanc].dev = dev;
	we->anc[we->nanc].ino = ino;
	we->anc[we->nanc].path = frt_strdup(path);
	we->nanc++;
}

static void anc_pop(struct walkenv *we)
{
	we->nanc--;
	free(we->anc[we->nanc].path);
}

static void walk_children(struct walkenv *we, struct frt_dir *d, int depth);

/* Open `name` under `parent_fd` and walk its contents at `child_depth`. */
static void descend_into(struct walkenv *we, int parent_fd, const char *name, int child_depth,
			 struct entry *ent)
{
	struct frt_dir *child = NULL;
	if (frt_diropen_at(parent_fd, name, &child) != 0) {
		if (errno == ENOENT && we->opts->ignore_readdir_race)
			return; /* entry vanished between readdir and open */
		report_error(we, we->path.data, errno);
		return;
	}

	int pushed = 0;
	if (we->opts->follow == 1) {
		const struct frt_statinfo *st = entry_stat(ent, &we->ctx);
		if (st) {
			anc_push(we, st->dev, st->ino, we->path.data);
			pushed = 1;
		}
	}

	walk_children(we, child, child_depth);

	if (pushed)
		anc_pop(we);
	frt_dirclose(child);
}

/* Evaluate one entry (respecting -mindepth) and recurse if it is a descendable
 * directory (respecting pre/post order, -prune, -maxdepth, -xdev). */
static void process_entry(struct walkenv *we, struct entry *ent, int dirfd, const char *name,
			  int depth, int dir_id)
{
	const struct options *o = we->opts;
	int isdir = entry_is_dir(we, ent);

	/* -L loop: a directory pointing back to an ancestor is reported and skipped
	 * entirely (no eval, no descent) — matches fts FTS_DC handling. */
	if (isdir) {
		const char *anc = loop_ancestor(we, ent);
		if (anc) {
			report_loop(we, we->path.data, anc);
			return;
		}
	}

	if (o->depth_first) {
		/* post-order: contents first, then the directory. -prune is a no-op. */
		if (isdir && may_descend(we, ent, depth))
			descend_into(we, dirfd, name, depth + 1, ent);
		if (we->ctx.quit)
			return;
		if (depth >= o->mindepth) {
			/* recursion left ctx->dirfd/dir_id pointing at the (now-closed)
			 * child dir and may have realloc'd the path buffer; restore them
			 * for the post-order evaluation of this entry. */
			ent->path = we->path.data;
			we->ctx.dirfd = dirfd;
			we->ctx.dir_id = dir_id;
			we->ctx.statname = ent->name;
			we->ctx.prune = false;
			(void)eval_expr(we->expr, ent, &we->ctx);
		}
	} else {
		/* pre-order: the directory, then its contents. */
		we->ctx.prune = false;
		if (depth >= o->mindepth)
			(void)eval_expr(we->expr, ent, &we->ctx);
		if (we->ctx.quit)
			return;
		if (isdir && !we->ctx.prune && may_descend(we, ent, depth))
			descend_into(we, dirfd, name, depth + 1, ent);
	}
}

static void walk_children(struct walkenv *we, struct frt_dir *d, int depth)
{
	int dirfd = frt_dirfd(d);
	int my_id = ++we->dir_seq; /* unique id for this directory instance */
	struct frt_dirent de;
	int r;

	while ((r = frt_dirread(d, &de)) == 1) {
		struct arena_marker mark = arena_mark(&we->arena);

		struct entry *ent = entry_new(&we->arena, de.name, de.namelen, de.type);
		ent->depth = depth;
		ent->ino = de.ino;

		size_t oldlen = we->path.len;
		if (we->path.len == 0 || we->path.data[we->path.len - 1] != '/')
			dstr_appendc(&we->path, '/');
		size_t basepos = we->path.len;
		dstr_append(&we->path, de.name, de.namelen);

		ent->path = we->path.data;
		ent->pathlen = (uint32_t)we->path.len;
		ent->basepos = (uint32_t)basepos;

		we->ctx.dirfd = dirfd;
		we->ctx.dir_id = my_id;
		we->ctx.statname = ent->name;

		process_entry(we, ent, dirfd, de.name, depth, my_id);

		dstr_truncate(&we->path, oldlen);
		arena_rewind(&we->arena, mark);

		if (we->ctx.quit)
			break;
	}
	if (r < 0)
		report_error(we, we->path.data, errno);
	/* flush this directory's -execdir '+' batch before its fd closes. */
	we->ctx.dirfd = dirfd;
	frt_exec_flush_tree(we->expr, 1, my_id, &we->ctx);
}

int frt_walk(const char *root, const struct options *opts, const struct expr *expr,
	     struct dstr *out, int out_fd, int *exit_status)
{
	struct walkenv we;
	we.opts = opts;
	we.expr = expr;
	arena_init(&we.arena, 0);
	dstr_init(&we.path);
	we.anc = NULL;
	we.nanc = we.anccap = 0;
	we.dir_seq = 0;

	we.ctx.follow = opts->follow;
	we.ctx.dir_id = 0;
	we.ctx.root = root;
	we.ctx.root_len = (uint32_t)strlen(root);
	we.ctx.out = out;
	we.ctx.out_fd = out_fd;
	we.ctx.arena = &we.arena;
	we.ctx.exit_status = exit_status;
	we.ctx.prune = false;
	we.ctx.quit = false;

	dstr_appendz(&we.path, root);

	size_t blen;
	const char *bname = base_of(root, &blen);
	struct entry *ent = entry_new(&we.arena, bname, blen, FRT_UNKNOWN);
	ent->depth = 0;
	ent->path = we.path.data;
	ent->pathlen = (uint32_t)we.path.len;
	ent->basepos = (uint32_t)(bname - root);

	/* find always stats the start path to classify it (and -H/-L follow it). */
	we.ctx.dirfd = AT_FDCWD;
	we.ctx.statname = root;
	struct frt_statinfo *si = arena_alloc(&we.arena, sizeof *si);
	int follow_root = opts->follow != 0; /* -L and -H follow named start paths */
	if (frt_stat_at(AT_FDCWD, root, follow_root, si) < 0) {
		/* -L/-H start point that won't follow (e.g. a broken symlink): fall back
		 * to lstat and treat it as the symlink itself (find does this; no error,
		 * rc stays 0, and -type l then matches). */
		if (follow_root && frt_stat_at(AT_FDCWD, root, 0, si) == 0) {
			follow_root = 0; /* classified as the link, not its target */
		} else {
			report_error(&we, root, errno);
			goto done;
		}
	}
	ent->st = si;
	ent->flags |= ENT_STATTED;
	ent->type = (uint16_t)frt_type_from_mode(si->mode);
	ent->ino = si->ino;
	ent->dev = si->dev;
	we.start_dev = si->dev;

	int isdir = (ent->type == FRT_DIR);
	if (opts->depth_first) {
		if (isdir && may_descend(&we, ent, 0)) {
			struct frt_dir *d = NULL;
			if (frt_diropen(root, &d) != 0)
				report_error(&we, root, errno);
			else {
				if (opts->follow == 1)
					anc_push(&we, si->dev, si->ino, root);
				walk_children(&we, d, 1);
				if (opts->follow == 1)
					anc_pop(&we);
				frt_dirclose(d);
			}
		}
		if (!we.ctx.quit && 0 >= opts->mindepth) {
			ent->path = we.path.data; /* walk may have realloc'd the buffer */
			we.ctx.dirfd = AT_FDCWD; /* recursion changed it; root stats from cwd */
			we.ctx.statname = root;
			we.ctx.prune = false;
			(void)eval_expr(expr, ent, &we.ctx);
		}
	} else {
		we.ctx.prune = false;
		if (0 >= opts->mindepth)
			(void)eval_expr(expr, ent, &we.ctx);
		if (!we.ctx.quit && isdir && !we.ctx.prune && may_descend(&we, ent, 0)) {
			struct frt_dir *d = NULL;
			if (frt_diropen(root, &d) != 0)
				report_error(&we, root, errno);
			else {
				if (opts->follow == 1)
					anc_push(&we, si->dev, si->ino, root);
				walk_children(&we, d, 1);
				if (opts->follow == 1)
					anc_pop(&we);
				frt_dirclose(d);
			}
		}
	}

done:
	free(we.anc);
	dstr_free(&we.path);
	arena_destroy(&we.arena);
	return we.ctx.quit;
}
