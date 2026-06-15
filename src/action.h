#ifndef FRT_ACTION_H
#define FRT_ACTION_H

#include "expr.h"

/* Output actions. -print / -print0 write the path to the buffered output. */
bool act_print(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool act_print0(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* -delete: remove the entry (post-order, -depth implied). -quit: stop the walk. */
bool act_delete(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool act_quit(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* Drain the output buffer to its fd if it has grown past the soft threshold. */
void out_maybe_flush(struct evalctx *ctx);
/* Drain whatever remains (call once at the end of the run). */
void out_flush(struct dstr *out, int fd);

#endif /* FRT_ACTION_H */
