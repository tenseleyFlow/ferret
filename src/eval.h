#ifndef FRT_EVAL_H
#define FRT_EVAL_H

#include "expr.h"

/* Evaluate the expression against one entry with short-circuit semantics.
 * Actions execute their side effects in evaluation order. */
bool eval_expr(const struct expr *e, struct entry *ent, struct evalctx *ctx);

#endif /* FRT_EVAL_H */
