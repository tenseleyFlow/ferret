#include "pred.h"
#include "glob.h"
#include "sys/dir.h"

#include <unistd.h>

bool pred_name(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)ctx;
	/* -name/-iname test the basename (ent->name). */
	return frt_glob_match(e->u.name.pattern, ent->name, e->u.name.glob_flags);
}

bool pred_type(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	if (ent->type == FRT_UNKNOWN) {
		if (!entry_stat(ent, ctx))
			return false; /* stat failed; can't classify */
	}
	return (e->u.type.mask & (1u << ent->type)) != 0;
}

/* -empty: true for a zero-length regular file or a directory with no entries
 * (read the directory — never trust st_size for dirs). Other types never match. */
bool pred_empty(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	if (ent->type == FRT_UNKNOWN)
		entry_stat(ent, ctx);

	if (ent->type == FRT_REG) {
		const struct frt_statinfo *st = entry_stat(ent, ctx);
		return st && st->size == 0;
	}
	if (ent->type == FRT_DIR) {
		struct frt_dir *d = NULL;
		if (frt_diropen_at(ctx->dirfd, ctx->statname, &d) != 0)
			return false;
		struct frt_dirent de;
		int r = frt_dirread(d, &de); /* 1 = has an entry, 0 = empty */
		frt_dirclose(d);
		return r == 0;
	}
	return false;
}

bool pred_true(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	(void)ent;
	(void)ctx;
	return true;
}

bool pred_false(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	(void)ent;
	(void)ctx;
	return false;
}

bool pred_prune(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	(void)ent;
	ctx->prune = true;
	return true;
}
