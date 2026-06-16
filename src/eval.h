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

/* Collect the top-level AND conjuncts of `root` that are evaluable from the
 * dirent alone (a stat-free necessary condition). Returns the count, capped at
 * `cap`. Used to pre-filter entries before the parallel-stat batch. */
int frt_expr_collect_guard(const struct expr *root, const struct expr **out, int cap);

/* True if a top-level conjunct is a -name/-iname filter (real selectivity). The
 * auto-engage heuristic leaves the pool off for such queries (serial is fine). */
int frt_expr_name_selective(const struct expr *root);

#endif /* FRT_EVAL_H */
