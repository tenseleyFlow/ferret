#include "test.h"
#include "sys/dir.h"
#include "config.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	char tmpl[] = "/tmp/frt_dir_test.XXXXXX";
	char *dir = mkdtemp(tmpl);
	CHECK("mkdtemp", dir != NULL);
	if (!dir)
		return test_summary("dir");

	char p[256];
	snprintf(p, sizeof p, "%s/regfile", dir);
	close(open(p, O_CREAT | O_WRONLY, 0644));
	snprintf(p, sizeof p, "%s/subdir", dir);
	mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/alink", dir);
	if (symlink("regfile", p) != 0) { /* some fs disallow; tolerate */
	}

	struct frt_dir *d = NULL;
	int rc = frt_diropen(dir, &d);
	CHECK("diropen", rc == 0 && d != NULL);

	int saw_reg = 0, saw_sub = 0, saw_lnk = 0, saw_dot = 0, n = 0;
	struct frt_dirent e;
	while (d && frt_dirread(d, &e) == 1) {
		n++;
		if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
			saw_dot++;
		if (strcmp(e.name, "regfile") == 0) {
			saw_reg = 1;
			CHECK("regfile namelen", e.namelen == 7);
#if FRT_HAS_D_TYPE
			CHECK("regfile type", e.type == FRT_REG || e.type == FRT_UNKNOWN);
#endif
		}
		if (strcmp(e.name, "subdir") == 0) {
			saw_sub = 1;
#if FRT_HAS_D_TYPE
			CHECK("subdir type", e.type == FRT_DIR || e.type == FRT_UNKNOWN);
#endif
		}
		if (strcmp(e.name, "alink") == 0)
			saw_lnk = 1;
	}
	CHECK("dot skipped", saw_dot == 0);
	CHECK("saw regfile", saw_reg == 1);
	CHECK("saw subdir", saw_sub == 1);
	(void)saw_lnk;
	if (d)
		frt_dirclose(d);

	/* cleanup */
	snprintf(p, sizeof p, "%s/regfile", dir);
	unlink(p);
	snprintf(p, sizeof p, "%s/alink", dir);
	unlink(p);
	snprintf(p, sizeof p, "%s/subdir", dir);
	rmdir(p);
	rmdir(dir);

	return test_summary("dir");
}
