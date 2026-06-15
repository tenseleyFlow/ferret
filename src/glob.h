#ifndef FRT_GLOB_H
#define FRT_GLOB_H

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

/* 1 if `text` matches `pattern`, else 0. */
int frt_glob_match(const char *pattern, const char *text, unsigned flags);

#endif /* FRT_GLOB_H */
