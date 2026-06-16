#include "outfile.h"
#include "diag.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define OUT_FLUSH_THRESHOLD (256u * 1024u) /* matches the stdout buffer threshold */

/* Drain o->buf to o->fd; record the first write errno. Leaves the buffer empty
 * (cleared, not freed) so it can keep accumulating. */
static void outfile_drain(struct outfile *o)
{
	size_t off = 0;
	while (off < o->buf.len) {
		ssize_t w = write(o->fd, o->buf.data + off, o->buf.len - off);
		if (w < 0) {
			if (!o->write_err)
				o->write_err = errno;
			break;
		}
		off += (size_t)w;
	}
	dstr_clear(&o->buf);
}

void frt_outfile_maybe_flush(struct outfile *o)
{
	if (o->buf.len >= OUT_FLUSH_THRESHOLD)
		outfile_drain(o);
}

struct outfile *frt_outfile_open(const char *path, struct outfile **list)
{
	for (struct outfile *o = *list; o; o = o->next)
		if (strcmp(o->path, path) == 0)
			return o; /* dedup: share one handle for repeated names */

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return NULL;
	struct outfile *o = frt_xmalloc(sizeof *o);
	o->path = frt_strdup(path);
	o->fd = fd;
	dstr_init(&o->buf);
	o->write_err = 0;
	o->next = *list;
	*list = o;
	return o;
}

void frt_outfile_flush_all(struct outfile *list, int *exit_status)
{
	for (struct outfile *o = list; o; o = o->next) {
		outfile_drain(o);
		if (o->write_err) {
			frt_diag_errno("", o->path, o->write_err);
			*exit_status = 1;
		}
		dstr_free(&o->buf);
		if (o->fd > 2)
			close(o->fd);
	}
}
