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
	case EXPR_AND:
		/* C && short-circuits: rhs (incl. its side effects) is skipped
		 * exactly when lhs is false. */
		return eval_expr(e->lhs, ent, ctx) && eval_expr(e->rhs, ent, ctx);
	case EXPR_OR:
		return eval_expr(e->lhs, ent, ctx) || eval_expr(e->rhs, ent, ctx);
	case EXPR_COMMA: {
		(void)eval_expr(e->lhs, ent, ctx); /* evaluated for side effects */
		return eval_expr(e->rhs, ent, ctx);
	}
	}
	return false; /* unreachable */
}

const struct frt_statinfo *entry_stat(struct entry *ent, struct evalctx *ctx)
{
	if (ent->flags & ENT_STATTED)
		return ent->st;
	if (ent->flags & ENT_STAT_FAILED)
		return NULL;

	struct frt_statinfo *si = arena_alloc(ctx->arena, sizeof *si);
	int follow = ctx->follow == 1; /* -L follows; -H/-P do not for descended entries */
	if (frt_stat_at(ctx->dirfd, ctx->statname, follow, si) < 0) {
		ent->flags |= ENT_STAT_FAILED;
		ent->stat_errno = errno;
		return NULL;
	}
	ent->st = si;
	ent->flags |= ENT_STATTED;
	/* Under -L the followed (target) type is what predicates see — a symlink's
	 * own type is irrelevant, so refine even when d_type already gave LNK. */
	if (follow || ent->type == FRT_UNKNOWN)
		ent->type = (uint16_t)frt_type_from_mode(si->mode);
	ent->ino = si->ino;
	ent->dev = si->dev;
	return si;
}
