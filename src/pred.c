#include "pred.h"
#include "glob.h"
#include "xregex.h"
#include "content.h"
#include "diag.h"
#include "idcache.h"
#include "sys/dir.h"
#include "sys/xstat.h"
#include "sys/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool pred_name(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)ctx;
	/* -name/-iname test the basename (ent->name). */
	return frt_glob_exec(e->u.name.glob_kind, e->u.name.pattern,
			     e->u.name.glob_litlen, ent->name, ent->namelen,
			     e->u.name.glob_flags);
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

/* find compares as uintmax_t (the stored value and the file's stat field are
 * both non-negative), so use unsigned arithmetic to match it exactly. */
static bool num_cmp(int kind, unsigned long long lhs, unsigned long long rhs)
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
	unsigned long long unit = (unsigned long long)e->u.size.unit;
	unsigned long long blocks = ((unsigned long long)st->size + unit - 1) / unit;
	return num_cmp(e->u.size.kind, blocks, e->u.size.val);
}

bool pred_links(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (unsigned long long)st->nlink, e->u.num.val);
}

bool pred_inum(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (unsigned long long)st->ino, e->u.num.val);
}

bool pred_uid(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (unsigned long long)st->uid, e->u.num.val);
}

bool pred_gid(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return num_cmp(e->u.num.kind, (unsigned long long)st->gid, e->u.num.val);
}

bool pred_nouser(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return frt_uid_name(st->uid) == NULL;
}

bool pred_nogroup(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;
	return frt_gid_name(st->gid) == NULL;
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
	/* conditional X resolves per file: execute iff a directory or already has
	 * any execute bit (gnulib mode_adjust). */
	int x = S_ISDIR(st->mode) || (st->mode & 0111);
	unsigned m = x ? e->u.perm.mode_x : e->u.perm.mode;
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

/* compare_ts: replicate find's exact timespec comparison (find/pred.c). */
static int ts_cmp(long long s1, long n1, long long s2, long n2)
{
	if (s1 == s2 && n1 == n2)
		return 0;
	double diff = difftime((time_t)s1, (time_t)s2) + 1.0e-9 * (double)(n1 - n2);
	return diff < 0.0 ? -1 : 1;
}

bool pred_path(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)ctx;
	/* -path/-ipath glob the whole path; '*' crosses '/'. */
	return frt_glob_exec(e->u.name.glob_kind, e->u.name.pattern,
			     e->u.name.glob_litlen, ent->path, ent->pathlen,
			     e->u.name.glob_flags);
}

bool pred_lname(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)ent;
	/* -lname/-ilname: glob the symlink target. Only matches actual symlinks. */
	char link[4096];
	ssize_t r = readlinkat(ctx->dirfd, ctx->statname, link, sizeof link - 1);
	if (r < 0)
		return false;
	link[r] = '\0';
	return frt_glob_exec(e->u.name.glob_kind, e->u.name.pattern,
			     e->u.name.glob_litlen, link, (size_t)r,
			     e->u.name.glob_flags);
}

bool pred_xtype(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	/* -xtype: stat with the opposite follow-sense of the mode. Default -P (and
	 * -H descended) follows the target; -L stats the link itself. */
	int xfollow = (ctx->follow != 1);
	struct frt_statinfo si;
	if (frt_stat_at(ctx->dirfd, ctx->statname, xfollow, &si) != 0) {
		if (xfollow && (errno == ENOENT || errno == ELOOP || errno == ENOTDIR)) {
			/* broken link: fall back to the link's own type (like ls -lL). */
			if (ent->type == FRT_UNKNOWN)
				entry_stat(ent, ctx);
			return (e->u.type.mask & (1u << ent->type)) != 0;
		}
		frt_diag_errno("", ent->path, errno);
		*ctx->exit_status = 1;
		return false;
	}
	enum frt_type t = frt_type_from_mode(si.mode);
	return (e->u.type.mask & (1u << t)) != 0;
}

bool pred_fstype(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)ent;
	return strcmp(frt_fstype(ctx->dirfd, ctx->statname), e->u.fstype) == 0;
}

bool pred_regex(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)ctx;
	return frt_regex_match(e->u.regex, ent->path, ent->pathlen);
}

/* -contains/-icontains (ferret extension): true if the file's content holds the
 * needle. Only regular files are inspected; everything else is false (no error).
 * An open/read failure is reported like find's other I/O errors (stderr, exit 1). */
bool pred_contains(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st || !S_ISREG(st->mode))
		return false;
	int err = 0;
	int hit = frt_file_contains(ctx->dirfd, ctx->statname, e->u.contains.needle,
				    e->u.contains.needle_len, e->u.contains.icase, &err);
	if (err) {
		frt_diag_errno("", ent->path, err);
		*ctx->exit_status = 1;
		return false;
	}
	return hit != 0;
}

bool pred_time(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return false;

	long long fsec;
	long fnsec;
	switch (e->u.time.field) {
	case TF_ATIME: fsec = st->atime; fnsec = st->atime_ns; break;
	case TF_CTIME: fsec = st->ctime; fnsec = st->ctime_ns; break;
	case TF_BTIME:
		if (!st->have_btime)
			return false;
		fsec = st->btime;
		fnsec = st->btime_ns;
		break;
	default:       fsec = st->mtime; fnsec = st->mtime_ns; break;
	}

	long long rsec = e->u.time.ref_sec;
	long rnsec = e->u.time.ref_nsec;
	switch (e->u.time.kind) {
	case COMP_GT:
		return ts_cmp(fsec, fnsec, rsec, rnsec) > 0;
	case COMP_LT:
		return ts_cmp(fsec, fnsec, rsec, rnsec) < 0;
	default: { /* COMP_EQ: a half-open window (reftime, reftime+window] */
		double delta = difftime((time_t)fsec, (time_t)rsec) +
			       1.0e-9 * (double)(fnsec - rnsec);
		return delta > 0.0 && delta <= (double)e->u.time.window;
	}
	}
}
