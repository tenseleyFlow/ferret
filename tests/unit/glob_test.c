#include "test.h"
#include "glob.h"

int main(void)
{
	CHECK("*.c matches a.c", frt_glob_match("*.c", "a.c", 0) == 1);
	CHECK("*.c no a.h", frt_glob_match("*.c", "a.h", 0) == 0);
	CHECK("a?c matches abc", frt_glob_match("a?c", "abc", 0) == 1);
	CHECK("a?c no ac", frt_glob_match("a?c", "ac", 0) == 0);
	CHECK("literal", frt_glob_match("file.txt", "file.txt", 0) == 1);

	/* find's -name has no FNM_PERIOD: '*' matches a leading dot */
	CHECK("* matches dotfile", frt_glob_match("*", ".hidden", 0) == 1);

	/* case folding (-iname) */
	CHECK("*.C casefold a.c", frt_glob_match("*.C", "a.c", FRT_GLOB_CASEFOLD) == 1);
	CHECK("*.C no fold a.c", frt_glob_match("*.C", "a.c", 0) == 0);

	/* bracket expressions */
	CHECK("[ab]* apple", frt_glob_match("[ab]*", "apple", 0) == 1);
	CHECK("[ab]* no cat", frt_glob_match("[ab]*", "cat", 0) == 0);

	return test_summary("glob");
}
