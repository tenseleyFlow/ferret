#ifndef FRT_EXPR_H
#define FRT_EXPR_H

/*
 * The expression AST and per-entry evaluation context. find's command line is an
 * expression tree of tests (predicates) and actions joined by operators; the
 * evaluator walks it per discovered entry with short-circuit semantics
 * (overview §4). Leaves carry an eval function pointer + payload; the optimizer
 * (sprint 09) reads the cost/prob/pure metadata.
 */

#include "entry.h"
#include "dstr.h"
#include "arena.h"

#include <stdbool.h>

struct expr;
struct evalctx;

typedef bool (*eval_fn)(const struct expr *e, struct entry *ent, struct evalctx *ctx);

enum expr_kind {
	EXPR_AND,   /* lhs -a rhs (juxtaposition or -a) */
	EXPR_OR,    /* lhs -o rhs */
	EXPR_NOT,   /* ! lhs */
	EXPR_COMMA, /* lhs , rhs (both evaluated, value of rhs) */
	EXPR_LEAF,  /* a predicate or action */
};

/* Predicate/action identity (optimizer + -D debug; one per leaf). */
enum pred_id {
	PRED_NAME, PRED_INAME, PRED_TYPE, PRED_EMPTY, PRED_TRUE, PRED_FALSE,
	ACT_PRINT, ACT_PRINT0,
};

struct expr {
	enum expr_kind kind;
	struct expr *lhs, *rhs; /* operands; NOT uses lhs, leaf uses neither */
	eval_fn eval;           /* leaf evaluator */
	enum pred_id pred;      /* leaf identity */

	/* optimizer metadata (overview §8, audit 03) */
	float cost, prob;
	bool needs_stat;        /* this leaf can trigger a stat */
	bool pure;              /* no side effects (safe to reorder) */
	bool no_default_print;  /* action that suppresses the implicit -print */

	union {
		struct { const char *pattern; unsigned glob_flags; } name;
		struct { unsigned mask; } type; /* bitmask over (1u << enum frt_type) */
	} u;
};

/* Per-entry evaluation context. The walker updates dirfd/statname/out as it goes;
 * evaluation is one entry at a time so this is single-threaded shared state. */
struct evalctx {
	int dirfd;             /* fd of the dir containing the entry (AT_FDCWD for roots) */
	const char *statname;  /* name to pass to fstatat under dirfd (root: full path) */
	int follow;            /* symlink follow mode: 0=-P 1=-L 2=-H */
	struct dstr *out;      /* output buffer */
	int out_fd;            /* where to drain the buffer (1 = stdout) */
	struct arena *arena;   /* for lazy stat allocation */
	int *exit_status;
};

/* stat-on-demand: fill ent->st (lstat or stat per follow), cached. NULL on
 * failure (sets ENT_STAT_FAILED + ent->stat_errno). Refines an UNKNOWN type. */
const struct frt_statinfo *entry_stat(struct entry *ent, struct evalctx *ctx);

#endif /* FRT_EXPR_H */
