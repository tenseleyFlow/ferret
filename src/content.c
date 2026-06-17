#include "content.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define CONTENT_BUF (64u * 1024u)

static unsigned char ascii_lower(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

/* First occurrence of needle in [hay, hay+hlen), or NULL. memchr skips to each
 * candidate first byte (the common-case fast path); the icase scan folds ASCII.
 * Caller guarantees nlen >= 1 and hlen >= nlen. */
static const char *find_bytes(const char *hay, size_t hlen, const char *needle,
			      size_t nlen, int icase)
{
	size_t last = hlen - nlen;
	if (!icase) {
		const char *p = hay;
		const char *end = hay + last;
		while (p <= end) {
			const char *q = memchr(p, needle[0], (size_t)(end - p) + 1);
			if (!q)
				return NULL;
			if (memcmp(q, needle, nlen) == 0)
				return q;
			p = q + 1;
		}
		return NULL;
	}
	unsigned char n0 = ascii_lower((unsigned char)needle[0]);
	for (size_t i = 0; i <= last; i++) {
		if (ascii_lower((unsigned char)hay[i]) != n0)
			continue;
		size_t j = 1;
		for (; j < nlen; j++)
			if (ascii_lower((unsigned char)hay[i + j]) !=
			    ascii_lower((unsigned char)needle[j]))
				break;
		if (j == nlen)
			return hay + i;
	}
	return NULL;
}

int frt_file_contains(int dirfd, const char *name, const char *needle,
		      size_t nlen, int icase, int *errp)
{
	*errp = 0;
	/* O_NOFOLLOW: the entry was stat-confirmed a regular file, so it should never
	 * be a symlink here; refuse to follow one swapped in after the check (TOCTOU). */
	int fd = openat(dirfd, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		*errp = errno;
		return 0;
	}
	/* Clear O_NONBLOCK so reads from a fifo/odd file block normally; it was set
	 * only so openat itself can't hang on a fifo with no writer. */
	int fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

	if (nlen == 0) { /* empty needle: any readable file matches (grep '' does) */
		close(fd);
		return 1;
	}

	/* Single eval thread (the pool only runs stat workers), so one reusable
	 * window buffer is safe and avoids a malloc per candidate file. */
	static char buf[CONTENT_BUF];
	int found = 0;
	size_t carry = 0; /* bytes retained from the previous chunk (overlap) */
	for (;;) {
		/* carry never reaches CONTENT_BUF (keep is capped below it), but pin the
		 * bound here so the read count is provably in [1, CONTENT_BUF] — else a
		 * fortified read() (glibc _FORTIFY_SOURCE) sees a possible SIZE_MAX. */
		if (carry >= CONTENT_BUF)
			carry = CONTENT_BUF - 1;
		size_t room = CONTENT_BUF - carry;
		ssize_t r = read(fd, buf + carry, room);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			*errp = errno;
			break;
		}
		size_t avail = carry + (size_t)r;
		if (avail >= nlen && find_bytes(buf, avail, needle, nlen, icase)) {
			found = 1;
			break;
		}
		if (r == 0)
			break; /* EOF, no match */
		/* Keep the last nlen-1 bytes so a needle straddling the boundary is
		 * still found once the next chunk lands after them. Cap below the
		 * window so the next read is always non-empty (a needle longer than the
		 * window only matches within a single window — fine for CLI-sized
		 * patterns, which is all -contains is meant for). */
		size_t keep = nlen - 1;
		if (keep > CONTENT_BUF - 1)
			keep = CONTENT_BUF - 1;
		if (keep > avail)
			keep = avail;
		memmove(buf, buf + avail - keep, keep);
		carry = keep;
	}

	close(fd);
	return found;
}
