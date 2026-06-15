#include "action.h"

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
