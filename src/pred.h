#ifndef FRT_PRED_H
#define FRT_PRED_H

#include "expr.h"

/* Test predicates. Each matches the eval_fn signature; the parser wires the
 * right one into a leaf and fills the payload. */
bool pred_name(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_type(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_empty(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_true(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_false(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* -prune: returns true and signals the walker not to descend into this dir
 * (no effect under -depth, handled by the walker). */
bool pred_prune(const struct expr *e, struct entry *ent, struct evalctx *ctx);

#endif /* FRT_PRED_H */
