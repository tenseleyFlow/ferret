#ifndef FRT_WALK_H
#define FRT_WALK_H

/*
 * Depth-first traversal engine (serial, pre-order). fd-relative, arena-backed
 * entries, stat only when a predicate forces it. Evaluates the expression
 * against each entry in find's order: the start path itself, then its contents
 * in readdir order (no sort), recursing pre-order (overview §11).
 */

#include "parse.h"
#include "expr.h"
#include "dstr.h"

/* Walk one start path. Appends output to `out` (drained to out_fd), sets
 * *exit_status to 1 on any error. Returns 1 if -quit fired (stop further roots). */
int frt_walk(const char *root, const struct options *opts, const struct expr *expr,
	     struct dstr *out, int out_fd, int *exit_status);

#endif /* FRT_WALK_H */
