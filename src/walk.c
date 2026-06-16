#include "walk.h"
#include "eval.h"
#include "entry.h"
#include "util.h"
#include "diag.h"
#include "exec.h"
#include "pool.h"
#include "iouring.h"
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

/* Cap on stat-free guard conjuncts kept for pre-filtering; extra ones just
 * aren't used as a guard (still evaluated by eval_expr — no correctness impact). */
#define FRT_GUARD_MAX 8

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
	struct frt_pool *pool;      /* parallel stat pool (NULL = serial/uring) */
	struct frt_iouring *uring;  /* io_uring stat backend (NULL = pool/serial) */
	int needs_stat;    /* expression can trigger a stat (gates the backend) */
	const struct expr *guard[FRT_GUARD_MAX]; /* stat-free necessary conditions */
	int nguard;        /* count of guard conjuncts (0 = no pre-filter) */
};

/* Don't dispatch to the pool for a survivor batch smaller than this — waking
 * worker threads costs more than the handful of serial fstatat calls it saves. */
#define FRT_POOL_BATCH_FLOOR 32

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

/* Open the directory containing a start path, so the root entry is evaluated in
 * the same parent-dir context find uses: -execdir/-okdir chdir there and run
 * "./basename", and %l/%F resolve relative to it. Returns a fd (caller closes if
 * >= 0) or AT_FDCWD when there's no meaningful parent or the open fails. */
static int open_start_parent(struct arena *a, const char *root)
{
	size_t n = strlen(root);
	while (n > 1 && root[n - 1] == '/') /* ignore trailing slashes */
		n--;
	size_t slash = 0;
	int has_slash = 0;
	for (size_t i = 0; i < n; i++)
		if (root[i] == '/') {
			slash = i;
			has_slash = 1;
		}
	const char *dir;
	if (!has_slash) {
		dir = "."; /* bare name: parent is the cwd */
	} else if (slash == 0) {
		dir = "/"; /* "/x": parent is the root dir */
	} else {
		char *b = arena_alloc(a, slash + 1);
		memcpy(b, root, slash);
		b[slash] = '\0';
		dir = b;
	}
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	return fd >= 0 ? fd : AT_FDCWD;
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

	walk_children(we, child, child_depth); /* consumes `child`: frees it, closes its fd */

	if (pushed)
		anc_pop(we);
}

/* Evaluate one entry (respecting -mindepth) and recurse if it is a descendable
 * directory (respecting pre/post order, -prune, -maxdepth, -xdev). */
static void process_entry(struct walkenv *we, struct entry *ent, int dirfd, const char *name,
			  int depth, int dir_id)
{
	const struct options *o = we->opts;
	int isdir = entry_is_dir(we, ent);

	/* -L: a symlink whose target can't be followed for a reason other than
	 * "doesn't exist" is an error find reports and skips (ELOOP, ENOTDIR,
	 * EACCES). entry_is_dir already triggered the follow-stat; a still-LNK type
	 * with a recorded failure means the target stat failed. Only a genuinely
	 * dangling link (ENOENT) is silently treated as the link itself. */
	if (we->ctx.follow == 1 && ent->type == FRT_LNK &&
	    (ent->flags & ENT_STAT_FAILED) && ent->stat_errno != ENOENT) {
		report_error(we, we->path.data, ent->stat_errno);
		return;
	}

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

/* Push the entry's name onto the path buffer, evaluate (and recurse), pop. */
static void process_one(struct walkenv *we, struct entry *ent, int dirfd, int depth, int my_id)
{
	size_t oldlen = we->path.len;
	if (we->path.len == 0 || we->path.data[we->path.len - 1] != '/')
		dstr_appendc(&we->path, '/');
	size_t basepos = we->path.len;
	dstr_append(&we->path, ent->name, ent->namelen);

	ent->path = we->path.data;
	ent->pathlen = (uint32_t)we->path.len;
	ent->basepos = (uint32_t)basepos;

	we->ctx.dirfd = dirfd;
	we->ctx.dir_id = my_id;
	we->ctx.statname = ent->name;

	process_entry(we, ent, dirfd, ent->name, depth, my_id);

	dstr_truncate(&we->path, oldlen);
}

/* Pool job: worker k stats the survivor entry surv[k] into slots[k] (workers
 * touch disjoint indices; fstatat on a shared dir fd is stateless — no locking).
 * surv maps the dense pool index to the entry index so only entries that pass
 * the stat-free guard are stat'd. */
struct statjob {
	struct entry **ents;
	struct frt_statinfo *slots;
	const uint32_t *surv;
	int dirfd;
	int follow;
};

static void stat_worker(void *arg, size_t k)
{
	struct statjob *j = arg;
	uint32_t i = j->surv[k];
	frt_entry_fill_stat(j->ents[i], j->dirfd, j->ents[i]->name, j->follow, &j->slots[k]);
}

/* Read every entry of `d` into the arena, then DROP the directory's 64KB read
 * buffer (frt_dir_release) while keeping its fd for fd-relative descent, and
 * evaluate/recurse in readdir order. Collecting first (like find's savedir)
 * means a deep chain costs entries-on-path, not 64KB * depth — so deep trees no
 * longer balloon virtual memory. Consumes `d` (frees it; closes the fd on
 * return). When the pool is engaged (-P, stat-heavy, --ferret-threads) the batch
 * is stat'd in parallel first; output order is identical for any thread count. */
static void walk_children(struct walkenv *we, struct frt_dir *d, int depth)
{
	int my_id = ++we->dir_seq; /* unique id for this directory instance */
	struct arena_marker dirmark = arena_mark(&we->arena);

	struct entry **ents = NULL;
	size_t cap = 0, n = 0;
	struct frt_dirent de;
	int r;
	while ((r = frt_dirread(d, &de)) == 1) {
		struct entry *ent = entry_new(&we->arena, de.name, de.namelen, de.type);
		ent->depth = depth;
		ent->ino = de.ino;
		if (n == cap) {
			cap = cap ? cap * 2 : 128;
			ents = frt_xrealloc(ents, frt_size_mul(cap, sizeof *ents));
		}
		ents[n++] = ent;
	}
	int read_err = (r < 0) ? errno : 0;

	/* Drop the 64KB buffer now; keep the fd open for descent into children. */
	int dirfd = frt_dir_release(d);
	if (dirfd < 0) {
		report_error(we, we->path.data, errno);
		free(ents);
		arena_rewind(&we->arena, dirmark);
		return;
	}
	if (read_err)
		report_error(we, we->path.data, read_err);

	if ((we->pool || we->uring) && we->needs_stat && we->ctx.follow == 0 && n > 0) {
		/* Pre-filter: only entries passing the stat-free guard can match, so
		 * only they need a stat. With no guard every entry survives (== the old
		 * stat-all behavior). The guard reads the entry name only — no path, and
		 * no stat (so it never serializes the walker; see guardable()). */
		uint32_t *surv = arena_alloc(&we->arena, n * sizeof *surv);
		size_t ns = 0;
		if (we->nguard > 0) {
			we->ctx.dirfd = dirfd;
			for (size_t i = 0; i < n; i++) {
				we->ctx.statname = ents[i]->name;
				int pass = 1;
				for (int g = 0; g < we->nguard && pass; g++)
					pass = eval_expr(we->guard[g], ents[i], &we->ctx);
				if (pass)
					surv[ns++] = (uint32_t)i;
			}
		} else {
			for (size_t i = 0; i < n; i++)
				surv[i] = (uint32_t)i;
			ns = n;
		}

		/* Batch-size floor: a tiny survivor set isn't worth backend dispatch —
		 * let eval_expr stat those few lazily on this thread. */
		if (ns >= FRT_POOL_BATCH_FLOOR) {
			struct frt_statinfo *slots = arena_alloc(&we->arena, ns * sizeof *slots);
			if (we->uring)
				frt_iouring_statx_batch(we->uring, dirfd, ents, surv, ns, 0, slots);
			else {
				struct statjob job = {ents, slots, surv, dirfd, 0};
				frt_pool_for(we->pool, ns, stat_worker, &job);
			}
		}
		for (size_t i = 0; i < n; i++) {
			process_one(we, ents[i], dirfd, depth, my_id);
			if (we->ctx.quit)
				break;
		}
	} else {
		/* Serial: per-entry arena rewind keeps scratch tight; the collected
		 * entry list itself sits below the per-entry mark and survives. */
		for (size_t i = 0; i < n; i++) {
			struct arena_marker em = arena_mark(&we->arena);
			process_one(we, ents[i], dirfd, depth, my_id);
			arena_rewind(&we->arena, em);
			if (we->ctx.quit)
				break;
		}
	}
	free(ents);

	/* flush this directory's -execdir '+' batch before its fd closes. */
	we->ctx.dirfd = dirfd;
	frt_exec_flush_tree(we->expr, 1, my_id, &we->ctx);
	close(dirfd);
	arena_rewind(&we->arena, dirmark);
}

int frt_walk(const char *root, const struct options *opts, const struct expr *expr,
	     struct frt_pool *pool, struct frt_iouring *uring, int needs_stat,
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
	we.pool = pool;
	we.uring = uring;
	we.needs_stat = needs_stat;
	/* Only worth a guard when a stat backend is engaged and the query can stat. */
	we.nguard = ((pool || uring) && needs_stat)
			    ? frt_expr_collect_guard(expr, we.guard, FRT_GUARD_MAX)
			    : 0;

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

	int root_parent_fd = AT_FDCWD; /* parent dir of the start point (for -execdir) */
	int root_dir_id = 0;

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
		int e = errno; /* capture before the lstat below clobbers errno */
		/* -L/-H start point that won't follow: only a genuinely dangling link
		 * (target ENOENT) falls back to lstat and is treated as the link itself
		 * (find: no error, rc 0, -type l matches). Any other failure (ELOOP,
		 * ENOTDIR, EACCES) is reported and skipped, like find. */
		if (follow_root && e == ENOENT && frt_stat_at(AT_FDCWD, root, 0, si) == 0) {
			follow_root = 0; /* classified as the link, not its target */
		} else {
			report_error(&we, root, e);
			goto done;
		}
	}
	ent->st = si;
	ent->flags |= ENT_STATTED;
	ent->type = (uint16_t)frt_type_from_mode(si->mode);
	ent->ino = si->ino;
	ent->dev = si->dev;
	we.start_dev = si->dev;

	/* Evaluate the start point in its parent-directory context (like find): so
	 * -execdir/-okdir chdir there and run "./basename", and %l/%F resolve
	 * relative to it. A unique dir_id keeps its '+' batch from merging with
	 * other start points. ent->path stays the full path for -print/plain -exec. */
	root_parent_fd = open_start_parent(&we.arena, root);
	root_dir_id = ++we.dir_seq;
	we.ctx.dirfd = root_parent_fd;
	we.ctx.statname = ent->name;
	we.ctx.dir_id = root_dir_id;

	int isdir = (ent->type == FRT_DIR);
	if (opts->depth_first) {
		if (isdir && may_descend(&we, ent, 0)) {
			struct frt_dir *d = NULL;
			if (frt_diropen(root, &d) != 0)
				report_error(&we, root, errno);
			else {
				if (opts->follow == 1)
					anc_push(&we, si->dev, si->ino, root);
				walk_children(&we, d, 1); /* consumes `d` */
				if (opts->follow == 1)
					anc_pop(&we);
			}
		}
		if (!we.ctx.quit && 0 >= opts->mindepth) {
			ent->path = we.path.data; /* walk may have realloc'd the buffer */
			we.ctx.dirfd = root_parent_fd; /* recursion changed it; restore */
			we.ctx.statname = ent->name;
			we.ctx.dir_id = root_dir_id;
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
				walk_children(&we, d, 1); /* consumes `d` */
				if (opts->follow == 1)
					anc_pop(&we);
			}
		}
	}

	/* flush the start point's own -execdir '+' batch in its parent dir. */
	we.ctx.dirfd = root_parent_fd;
	we.ctx.dir_id = root_dir_id;
	frt_exec_flush_tree(expr, 1, root_dir_id, &we.ctx);

done:
	if (root_parent_fd >= 0)
		close(root_parent_fd);
	free(we.anc);
	dstr_free(&we.path);
	arena_destroy(&we.arena);
	return we.ctx.quit;
}
