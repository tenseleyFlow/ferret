#ifndef FRT_UTIL_H
#define FRT_UTIL_H

#include <stddef.h>

/* Allocation that aborts on failure (CLI policy: OOM is fatal). Prints find's
 * gnulib diagnostic ("ferret: memory exhausted") and exit(1), program name aside. */
void *frt_xmalloc(size_t n);
void *frt_xrealloc(void *p, size_t n);

/* strdup via frt_xmalloc (so OOM is handled uniformly). */
char *frt_strdup(const char *s);

/* Saturating size arithmetic — clamp to SIZE_MAX on overflow instead of wrapping. */
size_t frt_size_add(size_t a, size_t b);
size_t frt_size_mul(size_t a, size_t b);

/* bit_width(0)=0, else floor(log2(x))+1.  bit_ceil(x)=smallest power of two >= x (>=1). */
unsigned frt_bit_width(size_t x);
size_t frt_bit_ceil(size_t x);

#endif /* FRT_UTIL_H */
