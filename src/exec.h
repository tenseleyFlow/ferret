#ifndef FRT_EXEC_H
#define FRT_EXEC_H

/*
 * -exec / -execdir / -ok / -okdir engine. ';' runs once per file (every {}
 * replaced); '+' batches paths into as few invocations as fit ARG_MAX (audit 02).
 * -execdir runs in the file's directory passing ./basename and batches per
 * directory. The batch state lives in the expr node and is flushed by the walker
 * at directory boundaries and at the very end.
 */

#include "expr.h"

/* Eval function for an ACT_EXEC leaf (matches eval_fn). */
bool act_exec(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* Allocate/compute the '+' batch for an exec node (call once after parse). */
void frt_exec_init(struct expr *e);

/* Flush pending '+' batches in the tree. execdir_only=1 flushes only -execdir
 * batches; dir_id>=0 flushes only batches for that directory instance (-1 = any). */
void frt_exec_flush_tree(const struct expr *e, int execdir_only, int dir_id, struct evalctx *ctx);

/* End-of-run flush of all pending '+' batches (call once after every root). */
void frt_exec_flush_pending(const struct expr *e, struct dstr *out, int out_fd,
			    int *exit_status);

/* Free the heap-allocated '+' batches in the tree (the CLI leaks them at exit;
 * tests and clean-shutdown paths call this). Safe once; clears each batch ptr. */
void frt_exec_free(struct expr *e);

#endif /* FRT_EXEC_H */
