#ifndef FRT_OUTFILE_H
#define FRT_OUTFILE_H

/*
 * Output-file destinations for -fprint/-fprint0/-fprintf/-fls. Each named file
 * is opened once (truncated) and shared across actions that name it (find's
 * sharefile), buffered, and drained at the end of the run.
 */

#include "dstr.h"

struct outfile {
	char *path;
	int fd;
	struct dstr buf;
	struct outfile *next;
};

/* Open `path` for writing (truncate), or return the existing handle if already
 * opened (dedup by path). Registers into *list. Returns NULL + errno on failure. */
struct outfile *frt_outfile_open(const char *path, struct outfile **list);

/* Drain every registered file's buffer to its fd and close them. */
void frt_outfile_flush_all(struct outfile *list);

#endif /* FRT_OUTFILE_H */
