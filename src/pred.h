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

/* Metadata predicates (each forces a stat on demand via entry_stat). */
bool pred_size(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_links(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_inum(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_uid(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_gid(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_nouser(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_nogroup(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_samefile(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_perm(const struct expr *e, struct entry *ent, struct evalctx *ctx);
bool pred_access(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* Time predicates: -atime -amin -ctime -cmin -mtime -mmin, the -newer family,
 * and -newerXY. The reference timestamp/kind/window are precomputed by the parser. */
bool pred_time(const struct expr *e, struct entry *ent, struct evalctx *ctx);

/* Advanced matching. */
bool pred_path(const struct expr *e, struct entry *ent, struct evalctx *ctx);   /* -path/-ipath */
bool pred_lname(const struct expr *e, struct entry *ent, struct evalctx *ctx);  /* -lname/-ilname */
bool pred_xtype(const struct expr *e, struct entry *ent, struct evalctx *ctx);  /* -xtype */
bool pred_fstype(const struct expr *e, struct entry *ent, struct evalctx *ctx); /* -fstype */
bool pred_regex(const struct expr *e, struct entry *ent, struct evalctx *ctx);  /* -regex/-iregex */

/* ferret extension: -contains/-icontains match a substring in the file content
 * (regular files only). High cost — the optimizer runs it after cheap filters. */
bool pred_contains(const struct expr *e, struct entry *ent, struct evalctx *ctx);

#endif /* FRT_PRED_H */
