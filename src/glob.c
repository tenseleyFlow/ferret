#include "glob.h"

#include <fnmatch.h>

/* find matches with fnmatch and backslash escapes active. -name/-path do not set
 * FNM_PATHNAME or FNM_PERIOD, so '*' crosses '/' and matches a leading dot. The
 * only flag find toggles is case folding (-iname/-ipath -> FNM_CASEFOLD). */
int frt_glob_match(const char *pattern, const char *text, unsigned flags)
{
	int f = 0;
#ifdef FNM_CASEFOLD
	if (flags & FRT_GLOB_CASEFOLD)
		f |= FNM_CASEFOLD;
#endif
	return fnmatch(pattern, text, f) == 0;
}
