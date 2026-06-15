#include "test.h"
#include "dstr.h"

int main(void)
{
	struct dstr s;
	dstr_init(&s);

	dstr_appendz(&s, "foo");
	CHECK_SZ("len after foo", s.len, 3);
	CHECK_STR("foo", s.data, "foo");

	dstr_appendc(&s, '/');
	dstr_append(&s, "bar", 3);
	CHECK_STR("foo/bar", s.data, "foo/bar");
	CHECK_SZ("len foo/bar", s.len, 7);

	/* truncate (path pop) */
	dstr_truncate(&s, 3);
	CHECK_STR("truncated", s.data, "foo");
	CHECK_SZ("len truncated", s.len, 3);

	dstr_clear(&s);
	CHECK_SZ("len cleared", s.len, 0);
	CHECK_STR("cleared", s.data, "");

	/* growth: append a large run */
	for (int i = 0; i < 5000; i++)
		dstr_appendc(&s, 'x');
	CHECK_SZ("len 5000", s.len, 5000);
	CHECK("nul terminated", s.data[5000] == '\0');

	dstr_free(&s);
	return test_summary("dstr");
}
