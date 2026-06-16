#include "eval.h"
#include "sys/xstat.h"

#include <errno.h>

bool eval_expr(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	switch (e->kind) {
	case EXPR_LEAF:
		return e->eval(e, ent, ctx);
	case EXPR_NOT:
		return !eval_expr(e->lhs, ent, ctx);
	case EXPR_AND: {
		bool l = eval_expr(e->lhs, ent, ctx);
		if (ctx->quit) /* -quit aborts immediately; don't run rhs */
			return l;
		return l && eval_expr(e->rhs, ent, ctx);
	}
	case EXPR_OR: {
		bool l = eval_expr(e->lhs, ent, ctx);
		if (ctx->quit)
			return l;
		return l || eval_expr(e->rhs, ent, ctx);
	}
	case EXPR_COMMA: {
		(void)eval_expr(e->lhs, ent, ctx); /* evaluated for side effects */
		if (ctx->quit)
			return false;
		return eval_expr(e->rhs, ent, ctx);
	}
	}
	return false; /* unreachable */
}

/* Fill `ent`'s metadata into the pre-allocated `slot` (no allocation — safe to
 * call from a pool worker, one entry per worker). Sets the STATTED/FAILED flag. */
void frt_entry_fill_stat(struct entry *ent, int dirfd, const char *statname, int follow,
			 struct frt_statinfo *slot)
{
	if (frt_stat_at(dirfd, statname, follow, slot) < 0) {
		ent->flags |= ENT_STAT_FAILED;
		ent->stat_errno = errno;
		return;
	}
	ent->st = slot;
	ent->flags |= ENT_STATTED;
	/* Under -L the followed (target) type is what predicates see — a symlink's
	 * own type is irrelevant, so refine even when d_type already gave LNK. */
	if (follow || ent->type == FRT_UNKNOWN)
		ent->type = (uint16_t)frt_type_from_mode(slot->mode);
	ent->ino = slot->ino;
	ent->dev = slot->dev;
}

int frt_expr_needs_stat(const struct expr *e)
{
	if (!e)
		return 0;
	if (e->kind == EXPR_LEAF)
		return e->needs_stat;
	return frt_expr_needs_stat(e->lhs) || frt_expr_needs_stat(e->rhs);
}

/* A subexpression is "guardable" if it can be evaluated from the entry name
 * alone, with no side effects and — critically — no stat: every leaf is a
 * -name/-iname/-true/-false test. Such a subexpr needs neither the built path
 * nor a stat, so it can pre-filter entries before the parallel-stat batch.
 *
 * -type is deliberately excluded: pred_type falls back to a stat when d_type is
 * DT_UNKNOWN (XFS ftype=0, many overlay/9p/FUSE mounts, the readdir backend), so
 * a -type guard would issue a serial fstatat per entry on the walker thread —
 * defeating the parallel batch the guard exists to feed. */
static int guardable(const struct expr *e)
{
	if (!e)
		return 1;
	if (e->kind == EXPR_LEAF) {
		switch (e->pred) {
		case PRED_NAME:
		case PRED_INAME:
		case PRED_TRUE:
		case PRED_FALSE:
			return 1;
		default:
			return 0;
		}
	}
	if (e->kind == EXPR_COMMA)
		return 0; /* its value/side-effect shape isn't a plain conjunct */
	return guardable(e->lhs) && guardable(e->rhs);
}

static int subtree_has_name(const struct expr *e)
{
	if (!e)
		return 0;
	if (e->kind == EXPR_LEAF)
		return e->pred == PRED_NAME || e->pred == PRED_INAME;
	return subtree_has_name(e->lhs) || subtree_has_name(e->rhs);
}

/* True if a top-level AND conjunct is a stat-free name filter (-name/-iname,
 * possibly inside a guardable subtree). A name pattern is a real selectivity
 * signal — most entries are rejected before any stat — so a serial walk is
 * already efficient and the auto-engage heuristic leaves the pool off. (-type
 * is deliberately not counted: it matches most entries, so it filters little.) */
int frt_expr_name_selective(const struct expr *root)
{
	if (!root)
		return 0;
	if (root->kind == EXPR_AND)
		return frt_expr_name_selective(root->lhs) ||
		       frt_expr_name_selective(root->rhs);
	return guardable(root) && subtree_has_name(root);
}

/* Collect into out[] the top-level AND conjuncts of `root` that are guardable.
 * Their conjunction is a necessary, stat-free condition for `root` to match:
 * an entry failing it can't match, so it never needs a stat. Returns the count
 * (capped at `cap`). The guard never changes output — it only decides which
 * entries are worth pre-stat'ing; eval_expr still runs in full afterwards. */
int frt_expr_collect_guard(const struct expr *root, const struct expr **out, int cap)
{
	if (!root || cap <= 0)
		return 0;
	if (root->kind == EXPR_AND) {
		int n = frt_expr_collect_guard(root->lhs, out, cap);
		return n + frt_expr_collect_guard(root->rhs, out + n, cap - n);
	}
	if (guardable(root)) {
		out[0] = root;
		return 1;
	}
	return 0;
}

const struct frt_statinfo *entry_stat(struct entry *ent, struct evalctx *ctx)
{
	if (ent->flags & ENT_STATTED)
		return ent->st;
	if (ent->flags & ENT_STAT_FAILED)
		return NULL;

	struct frt_statinfo *si = arena_alloc(ctx->arena, sizeof *si);
	frt_entry_fill_stat(ent, ctx->dirfd, ctx->statname, ctx->follow == 1, si);
	return (ent->flags & ENT_STATTED) ? ent->st : NULL;
}
