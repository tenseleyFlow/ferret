#ifndef FRT_ACTION_H
#define FRT_ACTION_H

#include "expr.h"

/* Output actions. -print/-print0/-fprint/-fprint0 write the path (+ newline or
 * NUL) to the action's destination (a -f* file, or stdout). */
bool act_print(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* The destination dstr for an output action (its -f* file buffer, or ctx->out). */
struct dstr *frt_out_dest(const struct expr *e, struct evalctx *ctx);

/* -delete: remove the entry (post-order, -depth implied). -quit: stop the walk. */
bool act_delete(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool act_quit(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* Drain the output buffer to its fd if it has grown past the soft threshold. */
void out_maybe_flush(struct evalctx *ctx);
/* Drain whatever remains (call once at the end of the run). */
void out_flush(struct dstr *out, int fd);
/* errno of the first stdout write failure (0 if none), checked after the final flush. */
int frt_out_write_errno(void);

#endif /* FRT_ACTION_H */
