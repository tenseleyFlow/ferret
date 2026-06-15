#include "pred.h"
#include "glob.h"
#include "sys/dir.h"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
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

static bool num_cmp(int kind, long long lhs, long long rhs)
{
	switch (kind) {
	case COMP_GT: return lhs > rhs;
	case COMP_LT: return lhs < rhs;
	default:      return lhs == rhs;
	}
}

bool pred_size(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	/* size compared in blocks of `unit`, rounded UP (a 1.5k file is -size 2k). */
	long long unit = e->u.size.unit;
	long long blocks = ((long long)st->size + unit - 1) / unit;
	return num_cmp(e->u.size.kind, blocks, e->u.size.val);
}

bool pred_links(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (long long)st->nlink, e->u.num.val);
}

bool pred_inum(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (long long)st->ino, e->u.num.val);
}

bool pred_uid(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (long long)st->uid, e->u.num.val);
}

bool pred_gid(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (long long)st->gid, e->u.num.val);
}

bool pred_nouser(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return getpwuid(st->uid) == NULL;
}

bool pred_nogroup(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return getgrgid(st->gid) == NULL;
}

bool pred_samefile(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return st->dev == e->u.samefile.dev && st->ino == e->u.samefile.ino;
}

bool pred_perm(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	unsigned bits = (unsigned)st->mode & 07777u;
	unsigned m = e->u.perm.mode;
	switch (e->u.perm.match) {
	case PERM_EXACT: return bits == m;
	case PERM_ALL:   return (bits & m) == m;
	case PERM_ANY:   return m == 0 ? true : (bits & m) != 0;
	}
	return false;
}

bool pred_access(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)ent;
	/* -readable/-writable/-executable: effective-id access check, fd-relative. */
	return faccessat(ctx->dirfd, ctx->statname, e->u.access.amode, AT_EACCESS) == 0;
}
