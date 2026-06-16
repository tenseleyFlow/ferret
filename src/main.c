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
	/* The parser builds the complete message (with find's wording, backticks,
	 * and locale-quoted values) at each error site, so this only adds the
	 * program prefix. */
	fprintf(stderr, "ferret: %s\n", pr->error);
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
	/* Clamp an explicit --ferret-threads N to the auto cap: more workers than
	 * that never help a stat pass and just burn ~2MB of stack each (a large N
	 * would spawn hundreds of threads). Output is identical for any N. */
	int max_workers = frt_pool_default_workers();
	if (threads > max_workers)
		threads = max_workers;
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
	int werr = frt_out_write_errno(); /* sticky across all stdout flushes */
	if (werr) {
		/* find reports both the stream-error line and the close-stdout line. */
		frt_diag_errno("", "standard output", werr);
		fprintf(stderr, "ferret: write error: %s\n", strerror(werr));
		exit_status = 1;
	}
	frt_outfile_flush_all(pr.outfiles, &exit_status); /* -fprint/-fprintf/-fls dests */
	dstr_free(&out);
	arena_destroy(&arena);
	return exit_status;
}
