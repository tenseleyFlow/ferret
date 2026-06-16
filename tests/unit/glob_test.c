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

	/* fast-path classification */
	unsigned ll = 999;
	CHECK("classify literal", frt_glob_classify("file.txt", 0, &ll) == FRT_GLOB_LITERAL && ll == 8);
	CHECK("classify suffix", frt_glob_classify("*.txt", 0, &ll) == FRT_GLOB_SUFFIX && ll == 4);
	CHECK("classify prefix", frt_glob_classify("file*", 0, &ll) == FRT_GLOB_PREFIX && ll == 4);
	CHECK("classify general star?", frt_glob_classify("*.t?t", 0, &ll) == FRT_GLOB_GENERAL);
	CHECK("classify general midstar", frt_glob_classify("a*b", 0, &ll) == FRT_GLOB_GENERAL);
	CHECK("classify general bracket", frt_glob_classify("[ab]*", 0, &ll) == FRT_GLOB_GENERAL);
	CHECK("classify general twostar", frt_glob_classify("*a*", 0, &ll) == FRT_GLOB_GENERAL);
	CHECK("classify escape general", frt_glob_classify("a\\*b", 0, &ll) == FRT_GLOB_GENERAL);
	CHECK("classify bare star suffix", frt_glob_classify("*", 0, &ll) == FRT_GLOB_SUFFIX && ll == 0);
	/* casefold + high byte refuses the fast path (fnmatch folds by locale) */
	CHECK("classify casefold ascii ok",
	      frt_glob_classify("*.TXT", FRT_GLOB_CASEFOLD, &ll) == FRT_GLOB_SUFFIX);
	CHECK("classify casefold highbyte general",
	      frt_glob_classify("*\xc3\x89.TXT", FRT_GLOB_CASEFOLD, &ll) == FRT_GLOB_GENERAL);

	/* fast-path exec matches fnmatch */
	CHECK("exec literal hit", frt_glob_exec(FRT_GLOB_LITERAL, "file.txt", 8, "file.txt", 8, 0) == 1);
	CHECK("exec literal len miss", frt_glob_exec(FRT_GLOB_LITERAL, "file.txt", 8, "file.txt2", 9, 0) == 0);
	CHECK("exec suffix hit", frt_glob_exec(FRT_GLOB_SUFFIX, "*.txt", 4, "a.txt", 5, 0) == 1);
	CHECK("exec suffix short", frt_glob_exec(FRT_GLOB_SUFFIX, "*.txt", 4, ".tx", 3, 0) == 0);
	CHECK("exec prefix hit", frt_glob_exec(FRT_GLOB_PREFIX, "file*", 4, "file99", 6, 0) == 1);
	CHECK("exec prefix short", frt_glob_exec(FRT_GLOB_PREFIX, "file*", 4, "fil", 3, 0) == 0);
	CHECK("exec bare star all", frt_glob_exec(FRT_GLOB_SUFFIX, "*", 0, "anything", 8, 0) == 1);
	CHECK("exec casefold suffix", frt_glob_exec(FRT_GLOB_SUFFIX, "*.TXT", 4, "a.txt", 5, FRT_GLOB_CASEFOLD) == 1);

	return test_summary("glob");
}
