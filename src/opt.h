#ifndef FRT_OPT_H
#define FRT_OPT_H

/*
 * Expression optimizer. Reorders cheap/likely-to-short-circuit
 * predicates ahead of expensive ones to cut stat/exec work, and folds no-op
 * constants. Only side-effect-free (pure) predicates are reordered; actions and
 * control-flow predicates (-print, -exec, -delete, -prune, -quit) are pinned, so
 * observable output is identical across -O levels.
 *
 *   -O0  no reordering            -O2  cost-based reorder
 *   -O1  cost-based reorder       -O3  + constant folding (default)
 *   -O4  + (treated as O3)
 */

#include "expr.h"
#include "arena.h"

struct expr *frt_optimize(struct expr *e, int level, struct arena *a);

/* Dump an expression tree to stderr (for -D tree / -D opt). */
void frt_expr_dump(const struct expr *e, const char *label);

#endif /* FRT_OPT_H */
