#include "exec.h"
#include "action.h"
#include "diag.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* '+' batch accumulator (one per -exec/-execdir + node). */
struct exec_batch {
	char **argv;       /* [initial args..., file args...] */
	int argc;          /* current count (initial + files) */
	int alloc;
	int init_argc;     /* count of initial template args (before {}) */
	size_t cur_chars;  /* byte accounting (find: strlen+1 per arg, +pfx) */
	size_t init_chars;
	size_t arg_max;    /* byte limit: min(128K, ARG_MAX - env - 2048) */
	size_t max_argc;   /* count limit */
	int dirfd;         /* -execdir: dir to run in; -1 for -exec (cwd) */
	int dir_id;        /* -execdir: directory instance the batch belongs to */
	int pending;       /* file args accumulated since last flush */
};

/* find's effective limits (buildcmd.c): arg_max = min(128KiB, ARG_MAX - env -
 * 2048), clamped to >= POSIX min; max_argc = (ARG_MAX - env - 2048)/8 - 2. */
static void compute_limits(size_t *arg_max, size_t *max_argc)
{
	long am = sysconf(_SC_ARG_MAX);
	size_t posix_max = (am > 0) ? (size_t)am : 131072u;

	size_t envsz = 0;
	extern char **environ;
	for (char **e = environ; *e; e++)
		envsz += strlen(*e) + 1;

	size_t headroom = 2048u;
	size_t avail = (posix_max > envsz + headroom) ? posix_max - envsz - headroom : 4096u;

	size_t sensible = 128u * 1024u;
	size_t am2 = sensible;
	if (sensible > avail)
		am2 = avail;
	if (am2 < 4096u)
		am2 = 4096u;
	*arg_max = am2;
	*max_argc = avail / sizeof(char *) - 2u;
}

void frt_exec_init(struct expr *e)
{
	if (!e->u.exec.multiple) {
		e->u.exec.batch = NULL;
		return;
	}
	struct exec_batch *b = frt_xmalloc(sizeof *b);
	memset(b, 0, sizeof *b);
	compute_limits(&b->arg_max, &b->max_argc);

	/* initial args = template tokens before the trailing {} (the {} is replaced
	 * by the batch of files). For '+', exactly one {} as the last template tok. */
	int ninit = e->u.exec.ntmpl - 1; /* drop the trailing {} */
	if (ninit < 0)
		ninit = 0;
	b->alloc = ninit + 16;
	b->argv = frt_xmalloc((size_t)b->alloc * sizeof(char *));
	b->cur_chars = 0;
	for (int i = 0; i < ninit; i++) {
		b->argv[i] = e->u.exec.tmpl[i]; /* borrowed (template lives in arena) */
		b->cur_chars += strlen(e->u.exec.tmpl[i]) + 1;
	}
	b->init_argc = ninit;
	b->argc = ninit;
	b->init_chars = b->cur_chars;
	b->dirfd = -1;
	b->pending = 0;
	e->u.exec.batch = b;
}

/* Replace every "{}" in `tok` with `subst` (find ';' substring substitution). */
static char *subst_token(const char *tok, const char *subst)
{
	size_t sl = strlen(subst), out = 0, n = 0;
	for (const char *p = tok; *p; p++) {
		if (p[0] == '{' && p[1] == '}') {
			n++;
			p++;
		}
	}
	if (n == 0)
		return frt_strdup(tok);
	out = strlen(tok) + n * (sl - 2) + 1;
	char *r = frt_xmalloc(out);
	char *w = r;
	for (const char *p = tok; *p;) {
		if (p[0] == '{' && p[1] == '}') {
			memcpy(w, subst, sl);
			w += sl;
			p += 2;
		} else {
			*w++ = *p++;
		}
	}
	*w = '\0';
	return r;
}

/* Fork+exec argv. For -execdir, fchdir(dirfd) in the child. Returns child
 * exited-0. Flushes ferret's output first so interleaving matches find. */
static int run_argv(char **argv, int execdir, int dirfd, int close_stdin, struct evalctx *ctx)
{
	out_flush(ctx->out, ctx->out_fd);
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0) {
		frt_diag_errno("", argv[0], errno);
		*ctx->exit_status = 1;
		return 0;
	}
	if (pid == 0) {
		if (execdir && dirfd >= 0 && fchdir(dirfd) != 0)
			_exit(126);
		/* -ok/-okdir: find closes the child's stdin so it can't consume the
		 * input find reads prompts from. */
		if (close_stdin)
			close(0);
		execvp(argv[0], argv);
		int e = errno;
		/* same diagnostic + quoting as find (printed from the child). */
		frt_diag_errno("", argv[0], e);
		_exit(e == ENOENT ? 127 : 126);
	}
	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	/* find does NOT change its exit status for a non-zero (or unrunnable) child;
	 * the predicate just evaluates false. Only find's own errors set exit 1. */
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static char *exec_subst(const struct expr *e, struct entry *ent)
{
	if (e->u.exec.execdir) {
		size_t n = strlen(ent->name);
		char *s = frt_xmalloc(n + 3);
		s[0] = '.';
		s[1] = '/';
		memcpy(s + 2, ent->name, n + 1);
		return s;
	}
	return frt_strdup(ent->path);
}

/* gnulib yesno: read a line from stdin, rpmatch() it. Prompt already shown. */
static int ask_yes(const char *prog, const char *path)
{
	fflush(stderr);
	fprintf(stderr, "< %s ... %s > ? ", prog, path);
	fflush(stderr);
	char buf[512];
	if (!fgets(buf, sizeof buf, stdin))
		return 0;
	return rpmatch(buf) > 0;
}

static void batch_flush(struct exec_batch *b, struct evalctx *ctx)
{
	if (!b->pending)
		return;
	b->argv[b->argc] = NULL;
	/* '+' mode: unlike ';', find propagates a failing batch command to its own
	 * exit status (non-zero exit, signal death, or exec failure). */
	if (!run_argv(b->argv, b->dirfd >= 0, b->dirfd, 0, ctx)) /* '+' is never -ok */
		*ctx->exit_status = 1;
	for (int i = b->init_argc; i < b->argc; i++)
		free(b->argv[i]);
	b->argc = b->init_argc;
	b->cur_chars = b->init_chars;
	b->pending = 0;
}

bool act_exec(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	if (!e->u.exec.multiple) {
		/* ';' : one invocation per file, every {} replaced. */
		int n = e->u.exec.ntmpl;
		char **argv = frt_xmalloc((size_t)(n + 1) * sizeof(char *));
		char *subst = exec_subst(e, ent);
		for (int i = 0; i < n; i++)
			argv[i] = subst_token(e->u.exec.tmpl[i], subst);
		argv[n] = NULL;
		free(subst);

		int result;
		if (e->u.exec.ok && !ask_yes(argv[0], ent->path)) {
			result = 0;
		} else {
			result = run_argv(argv, e->u.exec.execdir, ctx->dirfd, e->u.exec.ok, ctx);
		}
		for (int i = 0; i < n; i++)
			free(argv[i]);
		free(argv);
		return result != 0;
	}

	/* '+' : accumulate; flush when full. Always returns true (POSIX). */
	struct exec_batch *b = e->u.exec.batch;
	char *farg = exec_subst(e, ent);
	size_t flen = strlen(farg) + 1; /* find counts the NUL */

	/* -execdir batches per directory: flush when the file's directory changes
	 * (find runs the command in that directory). */
	if (b->pending && e->u.exec.execdir && b->dir_id != ctx->dir_id)
		batch_flush(b, ctx);
	if (b->pending &&
	    (b->cur_chars + flen > b->arg_max || (size_t)(b->argc + 1) > b->max_argc))
		batch_flush(b, ctx);

	b->dirfd = e->u.exec.execdir ? ctx->dirfd : -1;
	b->dir_id = ctx->dir_id;
	if (b->argc + 1 >= b->alloc) {
		/* grow in size_t so the doubling can't wrap the int before the cast */
		size_t na = (size_t)b->alloc * 2;
		b->argv = frt_xrealloc(b->argv, frt_size_mul(na, sizeof(char *)));
		b->alloc = (int)na;
	}
	b->argv[b->argc++] = farg;
	b->cur_chars += flen;
	b->pending++;
	return true;
}

void frt_exec_flush_tree(const struct expr *e, int execdir_only, int dir_id, struct evalctx *ctx)
{
	if (!e)
		return;
	if (e->kind == EXPR_LEAF) {
		if (e->pred == ACT_EXEC && e->u.exec.multiple && e->u.exec.batch) {
			struct exec_batch *b = e->u.exec.batch;
			if ((!execdir_only || e->u.exec.execdir) &&
			    (dir_id < 0 || b->dir_id == dir_id))
				batch_flush(b, ctx);
		}
		return;
	}
	frt_exec_flush_tree(e->lhs, execdir_only, dir_id, ctx);
	frt_exec_flush_tree(e->rhs, execdir_only, dir_id, ctx);
}

void frt_exec_flush_pending(const struct expr *e, struct dstr *out, int out_fd,
			    int *exit_status)
{
	/* Global -exec '+' batches run in the original cwd; no dir/arena needed. */
	struct evalctx ctx;
	memset(&ctx, 0, sizeof ctx);
	ctx.dirfd = AT_FDCWD;
	ctx.out = out;
	ctx.out_fd = out_fd;
	ctx.exit_status = exit_status;
	frt_exec_flush_tree(e, 0, -1, &ctx);
}
