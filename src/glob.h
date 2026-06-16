#ifndef FRT_GLOB_H
#define FRT_GLOB_H

#include <stddef.h>

/*
 * Glob matcher for -name/-iname (and later -path/-lname). find uses fnmatch
 * semantics with backslash escapes active (no FNM_NOESCAPE) and no FNM_PERIOD
 * (so '*' matches a leading dot, unlike a shell). -name matches the basename;
 * -path passes pathname=1 so '*' does NOT cross '/'.
 */

enum {
	FRT_GLOB_CASEFOLD = 1u << 0, /* -iname/-ipath: case-insensitive */
	FRT_GLOB_PATHNAME = 1u << 1, /* -path-style: '*' does not cross '/' is NOT set;
				      * find's -path lets '*' cross '/', so this stays off */
};

/* Pattern shapes the fast path recognises (frt_glob_classify). GENERAL is the
 * fnmatch fallback; the others compare a literal run directly. */
enum {
	FRT_GLOB_GENERAL = 0,
	FRT_GLOB_LITERAL,
	FRT_GLOB_SUFFIX,
	FRT_GLOB_PREFIX,
};

/* 1 if `text` matches `pattern`, else 0. */
int frt_glob_match(const char *pattern, const char *text, unsigned flags);

/* Classify `pattern` at parse time; returns one of the FRT_GLOB_* kinds and, for
 * the non-GENERAL kinds, the literal length to compare via *litlen. */
int frt_glob_classify(const char *pattern, unsigned flags, unsigned *litlen);

/* Match with a precomputed kind/litlen. textlen is the length of `text`. */
int frt_glob_exec(int kind, const char *pattern, unsigned litlen,
		  const char *text, size_t textlen, unsigned flags);

#endif /* FRT_GLOB_H */
