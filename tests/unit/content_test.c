#include "test.h"
#include "content.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Write `len` bytes to a fresh file under dirfd; returns its name (static). */
static const char *mkfile(int dirfd, const char *name, const void *data, size_t len)
{
	int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return NULL;
	ssize_t w = write(fd, data, len);
	close(fd);
	return (w == (ssize_t)len) ? name : NULL;
}

static int contains(int dirfd, const char *name, const char *needle, int icase)
{
	int err = 0;
	int r = frt_file_contains(dirfd, name, needle, strlen(needle), icase, &err);
	return err ? -1 : r;
}

int main(void)
{
	char tmpl[] = "/tmp/frt_content_XXXXXX";
	char *dir = mkdtemp(tmpl);
	CHECK("mkdtemp", dir != NULL);
	if (!dir)
		return test_summary("content");
	int d = open(dir, O_RDONLY | O_DIRECTORY);
	CHECK("open dir", d >= 0);

	mkfile(d, "a", "hello world\nfoo BAR\n", 20);
	CHECK("literal hit", contains(d, "a", "world", 0) == 1);
	CHECK("literal miss", contains(d, "a", "World", 0) == 0);
	CHECK("icase hit upper", contains(d, "a", "WORLD", 1) == 1);
	CHECK("icase hit mixed", contains(d, "a", "bar", 1) == 1);
	CHECK("icase miss", contains(d, "a", "baz", 1) == 0);
	CHECK("hit at start", contains(d, "a", "hello", 0) == 1);
	CHECK("empty needle matches", contains(d, "a", "", 0) == 1);

	/* needle straddling the 64KB window boundary */
	size_t big = 65534 + 8 + 100;
	char *buf = malloc(big);
	memset(buf, 'x', 65534);
	memcpy(buf + 65534, "BOUNDARY", 8);
	memset(buf + 65534 + 8, 'y', 100);
	mkfile(d, "b", buf, big);
	CHECK("straddles window", contains(d, "b", "BOUNDARY", 0) == 1);
	CHECK("straddles window icase", contains(d, "b", "boundary", 1) == 1);
	free(buf);

	/* binary content with embedded NULs */
	const unsigned char bin[] = {0x00, 0x01, 'S', 'E', 'C', 'R', 'E', 'T', 0x00, 0xff};
	mkfile(d, "c", bin, sizeof bin);
	CHECK("binary hit", contains(d, "c", "SECRET", 0) == 1);
	CHECK("binary miss", contains(d, "c", "SECRZT", 0) == 0);

	/* open error surfaces via *errp (returned as -1 here) */
	CHECK("missing file errors", contains(d, "nonexistent", "x", 0) == -1);

	/* cleanup */
	unlinkat(d, "a", 0);
	unlinkat(d, "b", 0);
	unlinkat(d, "c", 0);
	close(d);
	rmdir(dir);
	return test_summary("content");
}
