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
};

struct parse_result {
	char **paths; /* start paths (arena array; entries point into argv) */
	int npaths;
	struct expr *expr; /* full expression including any implicit -print */
	struct options opts;
	const char *error;     /* NULL on success; message body (no "ferret: " prefix) */
	const char *error_arg; /* offending token for messages that name one, else NULL */
};

/* Parse argv (argv[0] = program name). Expr/paths allocated from `arena`.
 * Returns 0 on success, -1 on parse error (out->error set). */
int frt_parse(int argc, char **argv, struct arena *arena, struct parse_result *out);

#endif /* FRT_PARSE_H */
