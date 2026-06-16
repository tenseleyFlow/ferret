#include "test.h"
#include "xregex.h"
#include "arena.h"

static int m(struct arena *a, const char *pat, int type, int fold, const char *s)
{
	const char *err = NULL;
	struct frt_regex *re = frt_regex_compile(pat, type, fold, a, &err);
	if (!re)
		return -1;
	int r = frt_regex_match(re, s, strlen(s));
	frt_regex_free(re); /* release engine state (struct stays in the arena) */
	return r;
}

int main(void)
{
	struct arena a;
	arena_init(&a, 0);

	/* whole-path anchoring: must match the entire string */
	CHECK("emacs .*\\.c full", m(&a, ".*\\.c", FRT_RE_EMACS, 0, "a/b.c") == 1);
	CHECK("emacs partial no match", m(&a, "b\\.c", FRT_RE_EMACS, 0, "a/b.c") == 0);
	CHECK("emacs exact", m(&a, "a/b\\.c", FRT_RE_EMACS, 0, "a/b.c") == 1);

	/* emacs alternation via \| */
	CHECK("emacs alt", m(&a, ".*\\.\\(c\\|h\\)", FRT_RE_EMACS, 0, "x.h") == 1);
	CHECK("emacs alt miss", m(&a, ".*\\.\\(c\\|h\\)", FRT_RE_EMACS, 0, "x.o") == 0);

	/* case fold */
	CHECK("iregex fold", m(&a, ".*\\.C", FRT_RE_EMACS, 1, "x.c") == 1);
	CHECK("regex nofold", m(&a, ".*\\.C", FRT_RE_EMACS, 0, "x.c") == 0);

	/* posix-egrep: bare () and | are operators */
	CHECK("egrep alt", m(&a, ".*/(a|b)", FRT_RE_POSIX_EGREP, 0, "x/a") == 1);

	/* posix-basic GNU BRE: \| alternation */
	CHECK("basic alt", m(&a, ".*\\.\\(c\\|h\\)", FRT_RE_POSIX_BASIC, 0, "x.c") == 1);

	/* regextype name mapping */
	CHECK("name emacs", frt_regextype_from_name("emacs") == FRT_RE_EMACS);
	CHECK("name egrep", frt_regextype_from_name("posix-egrep") == FRT_RE_POSIX_EGREP);
	CHECK("name bad", frt_regextype_from_name("nope") == -1);

	arena_destroy(&a);
	return test_summary("xregex");
}
