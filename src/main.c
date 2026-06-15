/* ferret — entry point. argv -> parse -> walk -> exit. */
#include "version.h"
#include "parse.h"
#include "walk.h"
#include "action.h"
#include "arena.h"
#include "dstr.h"

#include <locale.h>
#include <stdio.h>
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
	if (strcmp(pr->error, "unknown predicate") == 0 && pr->error_arg)
		fprintf(stderr, "ferret: unknown predicate `%s'\n", pr->error_arg);
	else if (strcmp(pr->error, "unknown argument to -type") == 0 && pr->error_arg)
		fprintf(stderr, "ferret: Unknown argument to -type: %s\n", pr->error_arg);
	else
		fprintf(stderr, "ferret: %s\n", pr->error);
}

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");

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

	struct dstr out;
	dstr_init(&out);
	int exit_status = 0;

	for (int i = 0; i < pr.npaths; i++)
		frt_walk(pr.paths[i], &pr.opts, pr.expr, &out, 1, &exit_status);

	out_flush(&out, 1);
	dstr_free(&out);
	arena_destroy(&arena);
	return exit_status;
}
