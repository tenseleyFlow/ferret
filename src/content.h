#ifndef FRT_CONTENT_H
#define FRT_CONTENT_H

#include <stddef.h>

/*
 * Content search for -contains/-icontains (a ferret extension; GNU find has no
 * content predicate). Streams the file in a bounded window with overlap so the
 * needle is found even across read boundaries and peak memory stays O(window),
 * never O(filesize). Binary-safe: the scan is over raw bytes, NUL included.
 * -icontains folds ASCII case only (locale-independent, like strncasecmp).
 */

/* Search the file at (dirfd, name) for `needle` (nlen bytes). Returns 1 if
 * present, 0 if absent. On an open/read error returns 0 and sets *errp to the
 * errno; otherwise *errp is 0. An empty needle matches any readable file. */
int frt_file_contains(int dirfd, const char *name, const char *needle,
		      size_t nlen, int icase, int *errp);

#endif /* FRT_CONTENT_H */
