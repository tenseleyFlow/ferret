#include "test.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>

int main(void)
{
	/* saturating add/mul */
	CHECK_SZ("add", frt_size_add(2, 3), 5);
	CHECK("add saturates", frt_size_add(SIZE_MAX, 1) == SIZE_MAX);
	CHECK_SZ("mul", frt_size_mul(4, 5), 20);
	CHECK("mul zero", frt_size_mul(0, 99) == 0);
	CHECK("mul saturates", frt_size_mul(SIZE_MAX, 2) == SIZE_MAX);

	/* bit width / ceil */
	CHECK_SZ("bit_width(0)", frt_bit_width(0), 0);
	CHECK_SZ("bit_width(1)", frt_bit_width(1), 1);
	CHECK_SZ("bit_width(255)", frt_bit_width(255), 8);
	CHECK("bit_ceil(0)=1", frt_bit_ceil(0) == 1);
	CHECK("bit_ceil(1)=1", frt_bit_ceil(1) == 1);
	CHECK("bit_ceil(17)=32", frt_bit_ceil(17) == 32);
	CHECK("bit_ceil(1024)=1024", frt_bit_ceil(1024) == 1024);

	char *s = frt_strdup("xyz");
	CHECK_STR("strdup", s, "xyz");
	free(s);

	return test_summary("util");
}
