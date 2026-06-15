/* -std=c11 sets __STRICT_ANSI__, which hides POSIX regcomp/regex_t on *BSD/macOS.
 * Linux gets _GNU_SOURCE from configure; ask for POSIX visibility elsewhere.
 * Must precede the first system header (arena.h -> <stddef.h>). */
#ifndef _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "xregex.h"
#include "util.h"

#include <regex.h>
#include <string.h>

struct frt_regex {
	regex_t re;
};

int frt_regextype_from_name(const char *name)
{
	if (strcmp(name, "emacs") == 0)
		return FRT_RE_EMACS;
	if (strcmp(name, "posix-awk") == 0 || strcmp(name, "awk") == 0)
		return FRT_RE_POSIX_AWK;
	if (strcmp(name, "posix-basic") == 0 || strcmp(name, "ed") == 0 ||
	    strcmp(name, "grep") == 0)
		return FRT_RE_POSIX_BASIC;
	if (strcmp(name, "posix-egrep") == 0 || strcmp(name, "egrep") == 0)
		return FRT_RE_POSIX_EGREP;
	if (strcmp(name, "posix-extended") == 0)
		return FRT_RE_POSIX_EXTENDED;
	return -1;
}

/* Translate emacs/GNU-BRE patterns to POSIX ERE by swapping the backslash sense
 * of the grouping/alternation metacharacters. emacs and GNU BRE both make bare
 * ( ) { } | literal and \( \) \{ \} \| special — the opposite of ERE. GNU BRE
 * additionally makes bare + ? literal and \+ \? operators (emacs treats bare
 * + ? as operators), so `bre` toggles that extra swap. */
static char *emacs_to_ere(const char *pat, int bre, struct arena *a)
{
	size_t n = strlen(pat);
	char *out = arena_alloc(a, n * 2 + 1);
	char *w = out;
	for (const char *p = pat; *p;) {
		if (*p == '\\' && p[1]) {
			char c = p[1];
			if (c == '(' || c == ')' || c == '{' || c == '}' || c == '|') {
				*w++ = c; /* \( -> ( etc. (now special in ERE) */
				p += 2;
				continue;
			}
			if (bre && (c == '+' || c == '?')) {
				*w++ = c; /* GNU BRE \+ -> ERE + (operator) */
				p += 2;
				continue;
			}
			*w++ = '\\'; /* keep other escapes verbatim (\. \\ \w ...) */
			*w++ = c;
			p += 2;
			continue;
		}
		if (*p == '(' || *p == ')' || *p == '{' || *p == '}' || *p == '|') {
			*w++ = '\\'; /* bare ( -> \( (literal in ERE) */
			*w++ = *p++;
			continue;
		}
		if (bre && (*p == '+' || *p == '?')) {
			*w++ = '\\'; /* GNU BRE bare + -> ERE \+ (literal) */
			*w++ = *p++;
			continue;
		}
		*w++ = *p++;
	}
	*w = '\0';
	return out;
}

struct frt_regex *frt_regex_compile(const char *pat, int regextype, int casefold,
				    struct arena *a, const char **errmsg)
{
	*errmsg = NULL;
	const char *compiled = pat;
	int cflags = 0;
	if (casefold)
		cflags |= REG_ICASE;

	switch (regextype) {
	case FRT_RE_POSIX_BASIC:
		/* find's posix-basic is GNU BRE (supports \| \( \) etc.). Translate
		 * to ERE with the BRE +/? literalness swap. */
		compiled = emacs_to_ere(pat, 1, a);
		cflags |= REG_EXTENDED;
		break;
	case FRT_RE_EMACS:
		compiled = emacs_to_ere(pat, 0, a);
		cflags |= REG_EXTENDED;
		break;
	default: /* awk / egrep / extended */
		cflags |= REG_EXTENDED;
		break;
	}

	struct frt_regex *r = arena_alloc(a, sizeof *r);
	int rc = regcomp(&r->re, compiled, cflags);
	if (rc != 0) {
		static char buf[256];
		regerror(rc, &r->re, buf, sizeof buf);
		*errmsg = buf;
		return NULL;
	}
	return r;
}

int frt_regex_match(const struct frt_regex *re, const char *path, size_t len)
{
	regmatch_t m;
	if (regexec(&re->re, path, 1, &m, 0) != 0)
		return 0;
	/* find anchors the whole path: the match must span [0, len). */
	return m.rm_so == 0 && m.rm_eo == (regoff_t)len;
}
