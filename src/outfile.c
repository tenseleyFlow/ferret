#include "outfile.h"
#include "util.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

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
	o->next = *list;
	*list = o;
	return o;
}

void frt_outfile_flush_all(struct outfile *list)
{
	for (struct outfile *o = list; o; o = o->next) {
		size_t off = 0;
		while (off < o->buf.len) {
			ssize_t w = write(o->fd, o->buf.data + off, o->buf.len - off);
			if (w < 0)
				break;
			off += (size_t)w;
		}
		dstr_free(&o->buf);
		if (o->fd > 2)
			close(o->fd);
	}
}
