#include "diag.h"

#include <langinfo.h>
#include <stdio.h>
#include <string.h>

static int g_utf8;

void frt_diag_init(void)
{
	g_utf8 = (strcmp(nl_langinfo(CODESET), "UTF-8") == 0);
}

int frt_diag_utf8(void)
{
	return g_utf8;
}

static void put_quoted(FILE *f, const char *path)
{
	if (g_utf8)
		fprintf(f, "\xe2\x80\x98%s\xe2\x80\x99", path);
	else
		fprintf(f, "'%s'", path);
}

void frt_diag_errno(const char *prefix, const char *path, int err)
{
	fputs("ferret: ", stderr);
	if (prefix)
		fputs(prefix, stderr);
	put_quoted(stderr, path);
	fprintf(stderr, ": %s\n", strerror(err));
}

void frt_diag_msg(const char *prefix, const char *path, const char *suffix)
{
	fputs("ferret: ", stderr);
	if (prefix)
		fputs(prefix, stderr);
	put_quoted(stderr, path);
	if (suffix)
		fprintf(stderr, ": %s", suffix);
	fputc('\n', stderr);
}
