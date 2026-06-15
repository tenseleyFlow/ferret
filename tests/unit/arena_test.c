#include "test.h"
#include "arena.h"

#include <string.h>

int main(void)
{
	struct arena a;
	arena_init(&a, 0);

	char *p = arena_alloc(&a, 16);
	CHECK("alloc non-null", p != NULL);
	memset(p, 0xab, 16); /* writable */

	char *s = arena_strdup(&a, "hello");
	CHECK_STR("strdup", s, "hello");

	/* alignment: arena_alloc is max_align_t aligned */
	void *q = arena_alloc(&a, 1);
	CHECK("aligned", ((size_t)q % _Alignof(max_align_t)) == 0);

	/* spanning a slab: allocate larger than default payload */
	char *big = arena_alloc(&a, 128 * 1024);
	CHECK("big alloc", big != NULL);
	memset(big, 1, 128 * 1024);

	/* mark/rewind: allocations after a mark are reclaimed */
	struct arena_marker m = arena_mark(&a);
	char *x = arena_strdup(&a, "scratch");
	CHECK_STR("scratch before rewind", x, "scratch");
	arena_rewind(&a, m);
	char *y = arena_strdup(&a, "reused");
	CHECK_STR("reused after rewind", y, "reused");

	arena_reset(&a);
	char *z = arena_alloc(&a, 8);
	CHECK("alloc after reset", z != NULL);

	arena_destroy(&a);
	return test_summary("arena");
}
