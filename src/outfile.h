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
	int write_err; /* errno of the first write failure, 0 if none */
	struct outfile *next;
};

/* Open `path` for writing (truncate), or return the existing handle if already
 * opened (dedup by path). Registers into *list. Returns NULL + errno on failure. */
struct outfile *frt_outfile_open(const char *path, struct outfile **list);

/* Drain this file's buffer if it has grown past the flush threshold (streaming,
 * so a large -f* run does not buffer the whole output in RAM). */
void frt_outfile_maybe_flush(struct outfile *o);

/* Drain every registered file's buffer to its fd and close them. On a write
 * failure, report find's diagnostic and set *exit_status to 1. */
void frt_outfile_flush_all(struct outfile *list, int *exit_status);

#endif /* FRT_OUTFILE_H */
