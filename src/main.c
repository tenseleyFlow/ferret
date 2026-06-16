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
#include "iouring.h"
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

	/* parallel stat pool (ferret extension). Engagement is structural, never
	 * probabilistic: the pool only helps when the walk is about to stat most
	 * entries, so it auto-engages exactly then — a stat-bound (-newer/-size/...)
	 * query, physical (-P) walk, with no cheap -name filter to reject entries
	 * first. A name-selective query rejects most entries before any stat, so a
	 * serial walk is already efficient and the pool stays off. The pool only
	 * stats; output is byte-identical to a serial run for any worker count.
	 *
	 *   opts.threads: -1 auto (default), 0 force-all, 1 serial, N = N workers.
	 * FRT_IO=uring requests the io_uring statx backend for the same stat batch
	 * (and, like before, forces engagement on); it falls back to the worker pool
	 * if io_uring is unavailable. Either backend produces byte-identical output. */
	int needs_stat = frt_expr_needs_stat(pr.expr);
	int want = pr.opts.threads;
	const char *io = getenv("FRT_IO");
	int prefer_uring = (io && strcmp(io, "uring") == 0);
	if (prefer_uring && want < 0)
		want = 0; /* explicit opt-in forces the parallel stat backend on */

	int max_workers = frt_pool_default_workers();
	int threads;
	if (want < 0) { /* auto: structural stat-bound, non-selective, physical */
		int engage = needs_stat && pr.opts.follow == 0 &&
			     !frt_expr_name_selective(pr.expr);
		threads = engage ? max_workers : 1;
	} else if (want == 0) {
		threads = max_workers; /* force-all */
	} else {
		/* Clamp explicit N to the auto cap: more workers than that never help a
		 * stat pass and just burn ~2MB of stack each. Output is identical. */
		threads = want > max_workers ? max_workers : want;
	}

	/* Engage a parallel stat backend when the walk will stat in bulk. Prefer
	 * io_uring when asked and available; otherwise the worker pool. */
	struct frt_pool *pool = NULL;
	struct frt_iouring *uring = NULL;
	if (threads > 1 && needs_stat) {
		if (prefer_uring)
			uring = frt_iouring_create(256); /* NULL if kernel/sandbox lacks it */
		if (!uring)
			pool = frt_pool_create(threads);
	}

	struct dstr out;
	dstr_init(&out);
	int exit_status = 0;

	for (int i = 0; i < pr.npaths; i++)
		if (frt_walk(pr.paths[i], &pr.opts, pr.expr, pool, uring, needs_stat, &out, 1,
			     &exit_status))
			break; /* -quit */

	frt_pool_destroy(pool);
	frt_iouring_destroy(uring);

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
