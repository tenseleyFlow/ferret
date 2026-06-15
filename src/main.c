/* ferret — entry point. argv -> parse -> walk -> exit. */
#include "version.h"
#include "parse.h"
#include "walk.h"
#include "action.h"
#include "exec.h"
#include "diag.h"
#include "outfile.h"
#include "opt.h"
#include "pool.h"
#include "eval.h"
#include "arena.h"
#include "dstr.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_version(void)
{
	printf("ferret %s\n", FRT_VERSION);
	printf("a fast, byte-for-byte GNU find(1) clone (parity target: findutils 4.10.0)\n");
}

static void print_usage(FILE *f)
{
	fprintf(f, "usage: ferret [-H|-L|-P] [-Olevel] [path...] [expression]\n");
}

static void report_parse_error(const struct parse_result *pr)
{
	const char *e = pr->error, *a = pr->error_arg;
	if (strcmp(e, "unknown predicate") == 0 && a)
		fprintf(stderr, "ferret: unknown predicate `%s'\n", a);
	else if (strcmp(e, "unknown argument to -type") == 0 && a)
		fprintf(stderr, "ferret: Unknown argument to -type: %s\n", a);
	else if (strcmp(e, "invalid -size type") == 0 && a)
		fprintf(stderr, "ferret: invalid -size type `%s'\n", a);
	else if (strcmp(e, "invalid mode") == 0 && a)
		fprintf(stderr, "ferret: invalid mode '%s'\n", a);
	else if (strcmp(e, "is not the name of a known user") == 0 && a)
		fprintf(stderr, "ferret: %s is not the name of a known user\n", a);
	else if (strcmp(e, "is not the name of a known group") == 0 && a)
		fprintf(stderr, "ferret: %s is not the name of a known group\n", a);
	else if (strcmp(e, "non-numeric argument") == 0 && a)
		fprintf(stderr, "ferret: invalid argument to %s\n", a);
	else
		fprintf(stderr, "ferret: %s\n", e);
}

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	frt_diag_init();

	if (argc >= 2) {
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-version") == 0) {
			print_version();
			return 0;
		}
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-help") == 0) {
			print_usage(stdout);
			return 0;
		}
	}

	struct arena arena;
	arena_init(&arena, 0);

	struct parse_result pr;
	if (frt_parse(argc, argv, &arena, &pr) != 0) {
		report_parse_error(&pr);
		arena_destroy(&arena);
		return 1;
	}

	if (pr.opts.debug & FRT_DBG_TREE)
		frt_expr_dump(pr.expr, "tree");
	pr.expr = frt_optimize(pr.expr, pr.opts.optlevel, &arena);
	if (pr.opts.debug & FRT_DBG_OPT)
		frt_expr_dump(pr.expr, "opt");

	/* parallel stat pool (ferret extension): only when stat-heavy and opted in.
	 * FRT_IO=uring requests the io_uring statx backend; until that lands it
	 * falls back to the worker pool (silently, so output is unaffected). */
	int needs_stat = frt_expr_needs_stat(pr.expr);
	int threads = pr.opts.threads;
	const char *io = getenv("FRT_IO");
	if (io && strcmp(io, "uring") == 0 && threads == 1)
		threads = 0; /* engage parallel stat (pool fallback) */
	if (threads == 0)
		threads = frt_pool_default_workers();
	struct frt_pool *pool = (threads > 1 && needs_stat) ? frt_pool_create(threads) : NULL;

	struct dstr out;
	dstr_init(&out);
	int exit_status = 0;

	for (int i = 0; i < pr.npaths; i++)
		if (frt_walk(pr.paths[i], &pr.opts, pr.expr, pool, needs_stat, &out, 1,
			     &exit_status))
			break; /* -quit */

	frt_pool_destroy(pool);

	/* run any pending -exec ... + batches accumulated across all roots */
	frt_exec_flush_pending(pr.expr, &out, 1, &exit_status);

	out_flush(&out, 1);
	frt_outfile_flush_all(pr.outfiles); /* -fprint/-fprintf/-fls destinations */
	dstr_free(&out);
	arena_destroy(&arena);
	return exit_status;
}
