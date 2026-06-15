#include "walk.h"
#include "eval.h"
#include "entry.h"
#include "sys/dir.h"
#include "sys/xstat.h"

#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct walkenv {
	const struct options *opts;
	const struct expr *expr;
	struct arena arena;
	struct dstr path;
	struct evalctx ctx;
	int utf8; /* locale uses UTF-8 quotes in diagnostics */
};

/* gnulib-style diagnostic: `ferret: <quoted-path>: <strerror>`. Quote chars are
 * locale-dependent (audit 01): U+2018/U+2019 under UTF-8, ASCII ' under C.
 * (Full quotearg escaping of control chars is deferred to a later sprint.) */
static void report_error(struct walkenv *we, const char *path, int err)
{
	if (we->utf8)
		fprintf(stderr, "ferret: \xe2\x80\x98%s\xe2\x80\x99: %s\n", path, strerror(err));
	else
		fprintf(stderr, "ferret: '%s': %s\n", path, strerror(err));
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

static void walk_children(struct walkenv *we, struct frt_dir *d, int depth);

/* Decide whether to descend into `ent`, resolving an UNKNOWN d_type by stat. */
static int is_dir_entry(struct entry *ent, struct walkenv *we)
{
	if (ent->type == FRT_DIR)
		return 1;
	if (ent->type == FRT_UNKNOWN) {
		entry_stat(ent, &we->ctx); /* refines ent->type */
		return ent->type == FRT_DIR;
	}
	return 0;
}

static void walk_children(struct walkenv *we, struct frt_dir *d, int depth)
{
	int dirfd = frt_dirfd(d);
	struct frt_dirent de;
	int r;

	while ((r = frt_dirread(d, &de)) == 1) {
		struct arena_marker mark = arena_mark(&we->arena);

		struct entry *ent = entry_new(&we->arena, de.name, de.namelen, de.type);
		ent->depth = depth;
		ent->ino = de.ino;

		/* push "<sep><name>" onto the path buffer */
		size_t oldlen = we->path.len;
		if (we->path.len == 0 || we->path.data[we->path.len - 1] != '/')
			dstr_appendc(&we->path, '/');
		size_t basepos = we->path.len;
		dstr_append(&we->path, de.name, de.namelen);

		ent->path = we->path.data;
		ent->pathlen = (uint32_t)we->path.len;
		ent->basepos = (uint32_t)basepos;

		we->ctx.dirfd = dirfd;
		we->ctx.statname = ent->name;

		(void)eval_expr(we->expr, ent, &we->ctx);

		if (is_dir_entry(ent, we)) {
			struct frt_dir *child = NULL;
			if (frt_diropen_at(dirfd, de.name, &child) != 0) {
				report_error(we, we->path.data, errno);
			} else {
				walk_children(we, child, depth + 1);
				frt_dirclose(child);
			}
		}

		dstr_truncate(&we->path, oldlen);
		arena_rewind(&we->arena, mark);
	}
	if (r < 0)
		report_error(we, we->path.data, errno);
}

void frt_walk(const char *root, const struct options *opts, const struct expr *expr,
	      struct dstr *out, int out_fd, int *exit_status)
{
	struct walkenv we;
	we.opts = opts;
	we.expr = expr;
	arena_init(&we.arena, 0);
	dstr_init(&we.path);
	we.utf8 = (strcmp(nl_langinfo(CODESET), "UTF-8") == 0);

	we.ctx.follow = opts->follow;
	we.ctx.out = out;
	we.ctx.out_fd = out_fd;
	we.ctx.arena = &we.arena;
	we.ctx.exit_status = exit_status;

	/* the start path itself (verbatim) */
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
		report_error(&we, root, errno);
		goto done;
	}
	ent->st = si;
	ent->flags |= ENT_STATTED;
	ent->type = (uint16_t)frt_type_from_mode(si->mode);
	ent->ino = si->ino;
	ent->dev = si->dev;

	(void)eval_expr(expr, ent, &we.ctx);

	if (ent->type == FRT_DIR) {
		struct frt_dir *d = NULL;
		if (frt_diropen(root, &d) != 0)
			report_error(&we, root, errno);
		else {
			walk_children(&we, d, 1);
			frt_dirclose(d);
		}
	}

done:
	dstr_free(&we.path);
	arena_destroy(&we.arena);
}
