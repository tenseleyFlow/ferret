#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* find's gnulib xalloc_die diagnostic + exit code (program name aside). */
static void oom(void)
{
	fputs("ferret: memory exhausted\n", stderr);
	exit(1);
}

void *frt_xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *frt_xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		oom();
	return q;
}

char *frt_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = frt_xmalloc(n);
	memcpy(d, s, n);
	return d;
}

size_t frt_size_add(size_t a, size_t b)
{
	size_t r = a + b;
	return r < a ? (size_t)-1 : r;
}

size_t frt_size_mul(size_t a, size_t b)
{
	if (a == 0 || b == 0)
		return 0;
	size_t r = a * b;
	return r / a != b ? (size_t)-1 : r;
}

unsigned frt_bit_width(size_t x)
{
	unsigned w = 0;
	while (x) {
		w++;
		x >>= 1;
	}
	return w;
}

size_t frt_bit_ceil(size_t x)
{
	if (x <= 1)
		return 1;
	return (size_t)1 << frt_bit_width(x - 1);
}
