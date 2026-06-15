#ifndef FRT_XREGEX_H
#define FRT_XREGEX_H

/*
 * Regex wrapper for -regex/-iregex. find's default dialect is `emacs`; the other
 * -regextype values map to POSIX BRE/ERE. The pattern matches the WHOLE path
 * (implicitly anchored). emacs is approximated by translating to POSIX ERE (the
 * backslash sense of ( ) { } | is swapped). See .docs/deviations.md for limits.
 */

#include "arena.h"

enum frt_regextype {
	FRT_RE_EMACS = 0, /* find default */
	FRT_RE_POSIX_AWK,
	FRT_RE_POSIX_BASIC,
	FRT_RE_POSIX_EGREP,
	FRT_RE_POSIX_EXTENDED,
};

struct frt_regex;

/* Map a -regextype NAME to the enum, or -1 if unknown. */
int frt_regextype_from_name(const char *name);

/* Compile `pat`. casefold => -iregex. Returns NULL and sets *errmsg on error.
 * The wrapper is arena-allocated; the underlying regex_t leaks at process exit
 * (CLI lifetime — see deviations). */
struct frt_regex *frt_regex_compile(const char *pat, int regextype, int casefold,
				    struct arena *a, const char **errmsg);

/* 1 if the whole [path, path+len) matches. */
int frt_regex_match(const struct frt_regex *re, const char *path, size_t len);

#endif /* FRT_XREGEX_H */
