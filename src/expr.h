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
	PRED_PRUNE, PRED_OPTION,
	PRED_SIZE, PRED_LINKS, PRED_INUM, PRED_UID, PRED_GID,
	PRED_NOUSER, PRED_NOGROUP, PRED_SAMEFILE, PRED_PERM, PRED_ACCESS,
	PRED_TIME,
	ACT_PRINT, ACT_PRINT0, ACT_EXEC, ACT_DELETE, ACT_QUIT,
};

/* Numeric comparison form for +N / -N / N arguments. */
enum comp_kind { COMP_GT, COMP_LT, COMP_EQ };

/* Which file timestamp a time predicate inspects. */
enum time_field { TF_ATIME, TF_MTIME, TF_CTIME, TF_BTIME };

/* -perm match modes. */
enum perm_match { PERM_EXACT, PERM_ALL, PERM_ANY };

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
		struct { int kind; long long val; } num; /* -links -inum -uid -gid -user -group */
		struct { int kind; long long val; long long unit; } size; /* -size */
		struct { int match; unsigned mode; } perm;                /* -perm */
		struct { dev_t dev; ino_t ino; } samefile;                /* -samefile */
		struct { int amode; } access;                             /* R_OK/W_OK/X_OK */
		struct {
			int kind;          /* COMP_GT/LT/EQ (already sense-inverted) */
			int field;         /* enum time_field: which file time */
			long long ref_sec; /* reference timestamp (computed at parse) */
			long ref_nsec;
			long window;       /* EQ window in seconds (DAYSECS or 60); 0 for -newer */
		} time;
		struct {
			char **tmpl;       /* command template tokens (incl. "{}") */
			int ntmpl;
			int multiple;      /* + batch mode (vs ; one-per-file) */
			int execdir;       /* run in the file's directory */
			int ok;            /* prompt on stderr / read stdin first */
			struct exec_batch *batch; /* + accumulator (heap), else NULL */
		} exec;
	} u;
};

/* Per-entry evaluation context. The walker updates dirfd/statname/out as it goes;
 * evaluation is one entry at a time so this is single-threaded shared state. */
struct evalctx {
	int dirfd;             /* fd of the dir containing the entry (AT_FDCWD for roots) */
	int dir_id;            /* unique id of the containing directory instance (-execdir +) */
	const char *statname;  /* name to pass to fstatat under dirfd (root: full path) */
	int follow;            /* symlink follow mode: 0=-P 1=-L 2=-H */
	struct dstr *out;      /* output buffer */
	int out_fd;            /* where to drain the buffer (1 = stdout) */
	struct arena *arena;   /* for lazy stat allocation */
	int *exit_status;
	bool prune;            /* set by -prune: walker skips descent into this dir */
	bool quit;             /* set by -quit: walker stops the whole search */
};

/* stat-on-demand: fill ent->st (lstat or stat per follow), cached. NULL on
 * failure (sets ENT_STAT_FAILED + ent->stat_errno). Refines an UNKNOWN type. */
const struct frt_statinfo *entry_stat(struct entry *ent, struct evalctx *ctx);

#endif /* FRT_EXPR_H */
