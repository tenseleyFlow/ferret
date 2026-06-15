#ifndef FRT_PARSE_H
#define FRT_PARSE_H

/*
 * Command line -> (global options, start paths, expression AST). Tokenizer +
 * recursive-descent parser with precedence climbing (audit 01). Implicit -print
 * is appended here when the expression contains no action.
 */

#include "expr.h"
#include "arena.h"

struct options {
	int follow;   /* 0=-P (default), 1=-L, 2=-H */
	int optlevel; /* -O level (default 3) */
	int maxdepth; /* -1 = unlimited */
	int mindepth; /* 0 = no floor */
	int depth_first;          /* -depth / -d: post-order traversal */
	int xdev;                 /* -xdev / -mount: do not cross device boundaries */
	int noleaf;               /* -noleaf: disable leaf optimization (no-op for now) */
	int ignore_readdir_race;  /* -ignore_readdir_race */
	int warn;                 /* -warn / -nowarn (default off when non-interactive) */
	int regextype;            /* -regextype: enum frt_regextype (default emacs) */
	unsigned debug;           /* -D bits: FRT_DBG_* */
	int threads;              /* --ferret-threads: 1=serial (default), 0=auto, N */
};

enum {
	FRT_DBG_TREE = 1u << 0, /* dump the parsed expression */
	FRT_DBG_OPT  = 1u << 1, /* dump the optimized expression */
};

struct parse_result {
	char **paths; /* start paths (arena array; entries point into argv) */
	int npaths;
	struct expr *expr; /* full expression including any implicit -print */
	struct options opts;
	struct outfile *outfiles; /* -f* destination files to flush at the end */
	const char *error;     /* NULL on success; message body (no "ferret: " prefix) */
	const char *error_arg; /* offending token for messages that name one, else NULL */
};

/* Parse argv (argv[0] = program name). Expr/paths allocated from `arena`.
 * Returns 0 on success, -1 on parse error (out->error set). */
int frt_parse(int argc, char **argv, struct arena *arena, struct parse_result *out);

#endif /* FRT_PARSE_H */
