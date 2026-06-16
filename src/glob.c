#include "glob.h"

#include <fnmatch.h>
#include <string.h>
#include <strings.h>

/* find matches with fnmatch and backslash escapes active. -name/-path do not set
 * FNM_PATHNAME or FNM_PERIOD, so '*' crosses '/' and matches a leading dot. The
 * only flag find toggles is case folding (-iname/-ipath -> FNM_CASEFOLD). */
int frt_glob_match(const char *pattern, const char *text, unsigned flags)
{
	int f = 0;
#ifdef FNM_CASEFOLD
	if (flags & FRT_GLOB_CASEFOLD)
		f |= FNM_CASEFOLD;
#endif
	return fnmatch(pattern, text, f) == 0;
}

/* Case folding via strncasecmp only matches fnmatch's FNM_CASEFOLD for ASCII;
 * fnmatch folds per the locale's towlower, which for bytes >= 0x80 in a UTF-8
 * locale is not strncasecmp's per-byte tolower. If the pattern is casefold and
 * carries any high byte, refuse the fast path and let fnmatch handle it. */
static int casefold_unsafe(const char *s, size_t n, unsigned flags)
{
	if (!(flags & FRT_GLOB_CASEFOLD))
		return 0;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)s[i] >= 0x80)
			return 1;
	return 0;
}

/* Classify a glob pattern so the matcher can skip fnmatch for the common shapes.
 *
 *   LITERAL  no metacharacters            -> exact compare
 *   SUFFIX   one leading '*', literal tail -> compare the tail
 *   PREFIX   one trailing '*', literal head -> compare the head
 *   GENERAL  anything else                -> fnmatch
 *
 * `*`, `?`, `[`, and `\` are the metacharacters find honours (no FNM_NOESCAPE).
 * On the LITERAL/SUFFIX/PREFIX paths *litlen receives the length of the literal
 * portion to compare; it is unused for GENERAL. A casefold pattern with any high
 * byte is forced to GENERAL (see casefold_unsafe). */
int frt_glob_classify(const char *pattern, unsigned flags, unsigned *litlen)
{
	size_t len = strlen(pattern);
	size_t meta = 0, stars = 0, first_star = 0, last_star = 0;
	int has_other_meta = 0;

	for (size_t i = 0; i < len; i++) {
		char c = pattern[i];
		if (c == '*') {
			if (!stars)
				first_star = i;
			last_star = i;
			stars++;
			meta++;
		} else if (c == '?' || c == '[' || c == '\\') {
			has_other_meta = 1;
			meta++;
		}
	}

	if (meta == 0) {
		if (casefold_unsafe(pattern, len, flags))
			return FRT_GLOB_GENERAL;
		*litlen = (unsigned)len;
		return FRT_GLOB_LITERAL;
	}

	/* Exactly one '*' and no other metachar: it is a pure prefix or suffix. */
	if (stars == 1 && !has_other_meta) {
		if (first_star == 0) { /* "*tail" */
			size_t taillen = len - 1;
			if (casefold_unsafe(pattern + 1, taillen, flags))
				return FRT_GLOB_GENERAL;
			*litlen = (unsigned)taillen;
			return FRT_GLOB_SUFFIX;
		}
		if (last_star == len - 1) { /* "head*" */
			size_t headlen = len - 1;
			if (casefold_unsafe(pattern, headlen, flags))
				return FRT_GLOB_GENERAL;
			*litlen = (unsigned)headlen;
			return FRT_GLOB_PREFIX;
		}
	}

	return FRT_GLOB_GENERAL;
}

/* Match using the precomputed classification. textlen avoids a strlen on the
 * hot path (callers know the basename/path length). For SUFFIX/PREFIX an empty
 * literal ("*") matches everything, matching fnmatch. */
int frt_glob_exec(int kind, const char *pattern, unsigned litlen,
		  const char *text, size_t textlen, unsigned flags)
{
	int fold = (flags & FRT_GLOB_CASEFOLD) != 0;

	switch (kind) {
	case FRT_GLOB_LITERAL:
		if (textlen != litlen)
			return 0;
		return fold ? strncasecmp(pattern, text, litlen) == 0
			    : memcmp(pattern, text, litlen) == 0;
	case FRT_GLOB_SUFFIX: {
		if (textlen < litlen)
			return 0;
		const char *tail = pattern + 1;
		const char *tt = text + (textlen - litlen);
		return fold ? strncasecmp(tail, tt, litlen) == 0
			    : memcmp(tail, tt, litlen) == 0;
	}
	case FRT_GLOB_PREFIX:
		if (textlen < litlen)
			return 0;
		return fold ? strncasecmp(pattern, text, litlen) == 0
			    : memcmp(pattern, text, litlen) == 0;
	default:
		return frt_glob_match(pattern, text, flags);
	}
}
