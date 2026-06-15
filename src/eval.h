#ifndef FRT_EVAL_H
#define FRT_EVAL_H

#include "expr.h"

/* Evaluate the expression against one entry with short-circuit semantics.
 * Actions execute their side effects in evaluation order. */
bool eval_expr(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* Fill ent->st into a caller-provided slot (no allocation; pool-worker safe). */
void frt_entry_fill_stat(struct entry *ent, int dirfd, const char *statname, int follow,
			 struct frt_statinfo *slot);

/* True if any leaf in the expression can trigger a stat (drives the pool). */
int frt_expr_needs_stat(const struct expr *e);

#endif /* FRT_EVAL_H */
