#ifndef FRT_FMT_H
#define FRT_FMT_H

/*
 * -printf / -fprintf / -fls format interpreter (sprint 07). A format string is
 * compiled once into a list of segments (literal text, \c stop, or a %directive
 * with its flags/width/precision and conversion). Rendering substitutes per
 * entry. Matches find's print.c: escapes, the directive table, ctime/strftime
 * formatting with the trailing-zero nanosecond suffix.
 */

#include "expr.h"
#include "arena.h"
#include "dstr.h"

struct fmt;

/* Compile FORMAT. On error returns NULL and sets *errmsg. */
struct fmt *fmt_compile(const char *format, struct arena *a, const char **errmsg);

/* 1 if any directive needs a stat (drives the optimizer's needs_stat). */
int fmt_needs_stat(const struct fmt *f);

/* Render the compiled format for `ent` into `out`; out_fd is where \c flushes. */
void fmt_render(const struct fmt *f, struct entry *ent, struct evalctx *ctx, struct dstr *out,
		int out_fd);

/* Action evals for -printf/-fprintf and -ls/-fls (destination from e->u.pf). */
bool act_printf(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool act_ls(const struct expr *e, struct entry *ent, struct evalctx *ctx);

#endif /* FRT_FMT_H */
