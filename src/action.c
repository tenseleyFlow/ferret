#include "action.h"
#include "diag.h"
#include "sys/dir.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Accumulate output and drain in large chunks (avoids per-entry write(2)). */
#define OUT_FLUSH_THRESHOLD (256u * 1024u)

void out_flush(struct dstr *out, int fd)
{
	size_t off = 0;
	while (off < out->len) {
		ssize_t w = write(fd, out->data + off, out->len - off);
		if (w < 0)
			break; /* output error; nothing useful to do mid-drain */
		off += (size_t)w;
	}
	dstr_clear(out);
}

void out_maybe_flush(struct evalctx *ctx)
{
	if (ctx->out->len >= OUT_FLUSH_THRESHOLD)
		out_flush(ctx->out, ctx->out_fd);
}

bool act_print(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	dstr_append(ctx->out, ent->path, ent->pathlen);
	dstr_appendc(ctx->out, '\n');
	out_maybe_flush(ctx);
	return true;
}

bool act_print0(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	dstr_append(ctx->out, ent->path, ent->pathlen);
	dstr_appendc(ctx->out, '\0');
	out_maybe_flush(ctx);
	return true;
}

bool act_delete(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	/* find never deletes the start point named "." (find/pred.c pred_delete). */
	if (strcmp(ctx->statname, ".") == 0)
		return true;
	if (ent->type == FRT_UNKNOWN)
		entry_stat(ent, ctx); /* resolve type so AT_REMOVEDIR is correct */
	int flag = ent->type == FRT_DIR ? AT_REMOVEDIR : 0;
	if (unlinkat(ctx->dirfd, ctx->statname, flag) == 0)
		return true;
	/* unlink() of an actual directory fails EISDIR — retry as rmdir. */
	if (errno == EISDIR && !(flag & AT_REMOVEDIR) &&
	    unlinkat(ctx->dirfd, ctx->statname, AT_REMOVEDIR) == 0)
		return true;
	frt_diag_errno("cannot delete ", ent->path, errno);
	*ctx->exit_status = 1;
	return false;
}

bool act_quit(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	(void)e;
	(void)ent;
	ctx->quit = true;
	return true;
}
