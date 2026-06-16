#include "fmt.h"
#include "eval.h"
#include "action.h"
#include "diag.h"
#include "outfile.h"
#include "util.h"
#include "sys/xstat.h"
#include "sys/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <math.h> /* HUGE_VAL (constant only; no libm link) */
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysmacros.h> /* major()/minor() (glibc moved them out of sys/types.h) */
#else
#include <sys/types.h>
#endif

enum seg_kind { SEG_PLAIN, SEG_STOP, SEG_DIR };
enum arg_type { ARG_STR, ARG_INT, ARG_UINT, ARG_DBL };

struct segment {
	enum seg_kind kind;
	const char *text; /* PLAIN/STOP literal bytes (arena) */
	size_t len;
	const char *spec; /* printf spec for a directive (arena, sized to fit) */
	char dirc;     /* directive char */
	char aux;      /* aux char for A/B/C/T strftime code */
	enum arg_type argtype;
};

struct fmt {
	struct segment *segs;
	int nseg;
	int needs_stat;
};

/* ---- helpers --------------------------------------------------------------- */

static const char *type_letter(enum frt_type t)
{
	switch (t) {
	case FRT_REG:  return "f";
	case FRT_DIR:  return "d";
	case FRT_LNK:  return "l";
	case FRT_SOCK: return "s";
	case FRT_BLK:  return "b";
	case FRT_CHR:  return "c";
	case FRT_FIFO: return "p";
	default:       return "U";
	}
}

static const char *mode_type_letter(mode_t m)
{
	return type_letter(frt_type_from_mode(m));
}

/* ls -l style 10-char mode string (gnulib filemodestring). */
static void filemodestring(mode_t mode, char *str)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:  str[0] = 'd'; break;
	case S_IFLNK:  str[0] = 'l'; break;
	case S_IFBLK:  str[0] = 'b'; break;
	case S_IFCHR:  str[0] = 'c'; break;
	case S_IFIFO:  str[0] = 'p'; break;
	case S_IFSOCK: str[0] = 's'; break;
	case S_IFREG:  str[0] = '-'; break;
	default:       str[0] = '?'; break;
	}
	str[1] = (mode & S_IRUSR) ? 'r' : '-';
	str[2] = (mode & S_IWUSR) ? 'w' : '-';
	str[3] = (mode & S_ISUID) ? ((mode & S_IXUSR) ? 's' : 'S')
				  : ((mode & S_IXUSR) ? 'x' : '-');
	str[4] = (mode & S_IRGRP) ? 'r' : '-';
	str[5] = (mode & S_IWGRP) ? 'w' : '-';
	str[6] = (mode & S_ISGID) ? ((mode & S_IXGRP) ? 's' : 'S')
				  : ((mode & S_IXGRP) ? 'x' : '-');
	str[7] = (mode & S_IROTH) ? 'r' : '-';
	str[8] = (mode & S_IWOTH) ? 'w' : '-';
	str[9] = (mode & S_ISVTX) ? ((mode & S_IXOTH) ? 't' : 'T')
				  : ((mode & S_IXOTH) ? 'x' : '-');
	str[10] = '\0';
}

static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* %a/%c/%t: "Www Mmm DD HH:MM:SS.nnnnnnnnn0 YYYY" (find ctime_format). */
static void ctime_format(time_t sec, long nsec, char *buf, size_t n)
{
	struct tm *tm = localtime(&sec);
	if (tm)
		snprintf(buf, n, "%3s %3s %2d %02d:%02d:%02d.%09ld0 %04d",
			 weekdays[tm->tm_wday], months[tm->tm_mon], tm->tm_mday,
			 tm->tm_hour, tm->tm_min, tm->tm_sec, nsec, 1900 + tm->tm_year);
	else
		snprintf(buf, n, "%lld.%09ld0", (long long)sec, nsec);
}

/* find's do_time_format: strftime, then splice the ns suffix in at the seconds
 * digits (detected by re-formatting a time with seconds offset by 11). */
static int scan_digit_diff(const char *a, const char *b, size_t *pos, size_t *len)
{
	size_t i = 0;
	while (a[i] && b[i] && a[i] == b[i])
		i++;
	if (!a[i] || !b[i])
		return 0;
	size_t j = i;
	while (a[j] && b[j] && a[j] != b[j] && a[j] >= '0' && a[j] <= '9' &&
	       b[j] >= '0' && b[j] <= '9')
		j++;
	*pos = i;
	*len = j - i;
	return *len > 0;
}

static void strftime_ns(char kind, time_t sec, long nsec, int want_ns, char *out, size_t n)
{
	struct tm *tm = localtime(&sec);
	if (!tm) {
		snprintf(out, n, "%lld", (long long)sec);
		return;
	}
	char fmt[16];
	if (kind == '+')
		strcpy(fmt, "%Y-%m-%d+%T");
	else {
		fmt[0] = '%';
		fmt[1] = kind;
		fmt[2] = '\0';
	}
	char nsbuf[32] = "";
	if (want_ns)
		snprintf(nsbuf, sizeof nsbuf, ".%09ld0", nsec);

	char base[256];
	if (strftime(base, sizeof base, fmt, tm) == 0 && fmt[1] != '\0') {
		out[0] = '\0';
		return;
	}
	if (!want_ns || !nsbuf[0]) {
		snprintf(out, n, "%s", base);
		return;
	}
	/* splice ns at the seconds digits */
	struct tm alt = *tm;
	alt.tm_sec = alt.tm_sec >= 11 ? alt.tm_sec - 11 : alt.tm_sec + 11;
	char altb[256];
	strftime(altb, sizeof altb, fmt, &alt);
	size_t pos, len;
	if (scan_digit_diff(base, altb, &pos, &len) && len == 2 &&
	    !(base[pos + len] >= '0' && base[pos + len] <= '9')) {
		snprintf(out, n, "%.*s%s%s", (int)(pos + len), base, nsbuf, base + pos + len);
	} else {
		snprintf(out, n, "%s", base);
	}
}

/* %T@/%A@ etc.: epoch seconds + ns suffix. */
static void epoch_ns(time_t sec, long nsec, char *out, size_t n)
{
	snprintf(out, n, "%lld.%09ld0", (long long)sec, nsec);
}

/* ---- compiler -------------------------------------------------------------- */

static enum arg_type dir_argtype(char c)
{
	if (c == 'd')
		return ARG_INT;
	if (c == 'm')
		return ARG_UINT;
	if (c == 'S')
		return ARG_DBL;
	return ARG_STR;
}

static char dir_conv(char c)
{
	if (c == 'd')
		return 'd';
	if (c == 'm')
		return 'o';
	if (c == 'S')
		return 'g';
	return 's';
}

static int dir_needs_stat(char c)
{
	switch (c) {
	case 'p': case 'f': case 'h': case 'P': case 'H': case 'd':
	case 'y': case 'l': case 'i': case '%':
		return 0; /* path/depth/type(d_type)/inode(d_ino)/linkname */
	default:
		return 1;
	}
}

/* The directives find's make_segment accepts. Anything else is unrecognized:
 * find warns and keeps the whole spec literal, and so do we. */
static int dir_known(char c)
{
	switch (c) {
	case '%': case 'a': case 'A': case 'b': case 'B': case 'c': case 'C':
	case 'd': case 'D': case 'f': case 'F': case 'g': case 'G': case 'h':
	case 'H': case 'i': case 'k': case 'l': case 'm': case 'M': case 'n':
	case 'p': case 'P': case 's': case 'S': case 't': case 'T': case 'u':
	case 'U': case 'y': case 'Y': case 'Z':
		return 1;
	default:
		return 0;
	}
}

struct fmt *fmt_compile(const char *format, struct arena *a, const char **errmsg)
{
	*errmsg = NULL;
	size_t flen = strlen(format);
	struct fmt *f = arena_alloc(a, sizeof *f);
	f->segs = arena_alloc(a, (flen + 1) * sizeof(struct segment));
	f->nseg = 0;
	f->needs_stat = 0;

	/* plain-text accumulator (arena buffer, big enough for the whole format) */
	char *plain = arena_alloc(a, flen + 1);
	size_t plen = 0;

	#define FLUSH_PLAIN()                                                  \
		do {                                                           \
			if (plen) {                                            \
				struct segment *s = &f->segs[f->nseg++];       \
				s->kind = SEG_PLAIN;                           \
				s->text = arena_memdup(a, plain, plen);        \
				s->len = plen;                                 \
				plen = 0;                                      \
			}                                                      \
		} while (0)

	const char *p = format;
	while (*p) {
		if (*p == '\\') {
			p++;
			char c = 0;
			int stop = 0;
			switch (*p) {
			case 'a': c = '\a'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'v': c = '\v'; break;
			case '\\': c = '\\'; break;
			case 'c': stop = 1; break;
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7': {
				int v = 0, k = 0;
				while (k < 3 && *p >= '0' && *p <= '7') {
					v = v * 8 + (*p - '0');
					p++;
					k++;
				}
				plain[plen++] = (char)v;
				continue;
			}
			case '\0':
				plain[plen++] = '\\';
				continue;
			default:
				/* unknown escape: warn (find does, at compile time) and
				 * keep the backslash and the char literally. */
				fprintf(stderr, "ferret: warning: unrecognized escape `\\%c'\n",
					*p);
				plain[plen++] = '\\';
				plain[plen++] = *p;
				p++;
				continue;
			}
			if (stop) {
				FLUSH_PLAIN();
				struct segment *s = &f->segs[f->nseg++];
				s->kind = SEG_STOP;
				s->text = NULL;
				s->len = 0;
				p++;
				continue;
			}
			plain[plen++] = c;
			p++;
			continue;
		}
		if (*p == '%') {
			p++;
			if (*p == '%') {
				plain[plen++] = '%';
				p++;
				continue;
			}
			if (*p == '\0') {
				*errmsg = "error: % at end of format string";
				return NULL;
			}
			/* Measure the flags/width/precision span, then allocate a
			 * spec buffer sized to fit it (find allocates its segment
			 * buffer the same way). No fixed cap, so pathological widths
			 * like %99999999999p can't overflow; they reach snprintf
			 * verbatim, exactly as find hands them to its printf. */
			const char *spec_src = p;
			const char *fp = p;
			while (*fp == '-' || *fp == '+' || *fp == ' ' || *fp == '#' || *fp == '0')
				fp++;
			while (*fp >= '0' && *fp <= '9')
				fp++;
			if (*fp == '.') {
				fp++;
				while (*fp >= '0' && *fp <= '9')
					fp++;
			}
			size_t fwlen = (size_t)(fp - p); /* flags+width+precision */
			char dirc = fp[0];
			/* find's directive dispatch: A/B/C/T take a trailing strftime
			 * char; the rest of dir_known are single-char; '{', '[', '(' and a
			 * directive cut off at end-of-string are fatal ("reserved for
			 * future use"); anything else is unrecognized (warn, keep the whole
			 * spec literal). */
			int is_time = dirc == 'A' || dirc == 'B' || dirc == 'C' || dirc == 'T';
			int len = is_time ? 2 : (dir_known(dirc) ? 1 : 0);
			if (!len || !fp[len - 1]) {
				if (dirc == '\0' || dirc == '{' || dirc == '[' || dirc == '(') {
					char *m = arena_alloc(a, 64);
					/* find prints a literal NUL byte here when the format
					 * ends mid-directive; emit a clean message instead. */
					if (dirc)
						snprintf(m, 64, "error: the format directive "
							 "`%%%c' is reserved for future use", dirc);
					else
						snprintf(m, 64, "error: the format directive "
							 "`%%' is reserved for future use");
					*errmsg = m;
					return NULL;
				}
				if (is_time)
					fprintf(stderr, "ferret: warning: format directive "
						"`%%%c' should be followed by another "
						"character\n", dirc);
				else
					fprintf(stderr, "ferret: warning: unrecognized "
						"format directive `%%%c'\n", dirc);
				plain[plen++] = '%';
				memcpy(plain + plen, spec_src, fwlen);
				plen += fwlen;
				plain[plen++] = dirc;
				p = fp + 1;
				continue;
			}
			char aux = is_time ? fp[1] : 0;
			char *spec = arena_alloc(a, fwlen + 3); /* '%' + span + conv + NUL */
			size_t si = 0;
			spec[si++] = '%';
			memcpy(spec + si, p, fwlen);
			si += fwlen;
			spec[si++] = dir_conv(dirc);
			spec[si] = '\0';
			p = fp + len; /* consume directive (+ strftime char if time) */

			FLUSH_PLAIN();
			struct segment *s = &f->segs[f->nseg++];
			s->kind = SEG_DIR;
			s->dirc = dirc;
			s->aux = aux;
			s->argtype = dir_argtype(dirc);
			s->spec = spec;
			if (dir_needs_stat(dirc))
				f->needs_stat = 1;
			continue;
		}
		plain[plen++] = *p++;
	}
	FLUSH_PLAIN();
	#undef FLUSH_PLAIN
	return f;
}

int fmt_needs_stat(const struct fmt *f)
{
	return f->needs_stat;
}

/* ---- renderer -------------------------------------------------------------- */

static void emit_spec_str(struct dstr *out, const char *spec, const char *val)
{
	char buf[512];
	int n = snprintf(buf, sizeof buf, spec, val);
	if (n < 0)
		return;
	if ((size_t)n < sizeof buf) {
		dstr_append(out, buf, (size_t)n);
	} else {
		char *big = frt_xmalloc((size_t)n + 1);
		snprintf(big, (size_t)n + 1, spec, val);
		dstr_append(out, big, (size_t)n);
		free(big);
	}
}

/* Numeric directives (%d/%m/%S) need the same measure-then-malloc as
 * emit_spec_str, so a wide field width is not truncated by a fixed buffer. A
 * macro because the argument type varies (int / unsigned / double). */
#define EMIT_SPEC_NUM(out, spec, val)                                          \
	do {                                                                   \
		char _b[512];                                                  \
		int _n = snprintf(_b, sizeof _b, (spec), (val));               \
		if (_n < 0)                                                    \
			break;                                                 \
		if ((size_t)_n < sizeof _b) {                                  \
			dstr_append((out), _b, (size_t)_n);                    \
		} else {                                                       \
			char *_big = frt_xmalloc((size_t)_n + 1);              \
			snprintf(_big, (size_t)_n + 1, (spec), (val));         \
			dstr_append((out), _big, (size_t)_n);                  \
			free(_big);                                            \
		}                                                              \
	} while (0)

static void render_dir(const struct segment *s, struct entry *ent, struct evalctx *ctx,
		       struct dstr *out)
{
	char val[1024];
	const struct frt_statinfo *st = NULL;
	if (dir_needs_stat(s->dirc) || s->dirc == 'Y')
		st = entry_stat(ent, ctx);

	switch (s->dirc) {
	case '%':
		dstr_appendc(out, '%');
		return;
	case 'p':
		emit_spec_str(out, s->spec, ent->path);
		return;
	case 'f': {
		/* find's %f is gnulib base_name(path): the last component, keeping any
		 * trailing slashes ("corpus/" -> "corpus/"); "/" for an all-slash path.
		 * It's a suffix of ent->path (NUL-terminated), so no copy is needed —
		 * and ent->name is wrong here since it has trailing slashes stripped. */
		const char *path = ent->path;
		size_t len = ent->pathlen, i = 0;
		while (i < len && path[i] == '/')
			i++;
		if (i == len) {
			emit_spec_str(out, s->spec, "/");
		} else {
			size_t base = i;
			int saw = 0;
			for (size_t k = i; k < len; k++) {
				if (path[k] == '/')
					saw = 1;
				else if (saw) {
					base = k;
					saw = 0;
				}
			}
			emit_spec_str(out, s->spec, path + base);
		}
		return;
	}
	case 'h': {
		/* find's %h: strip trailing slashes (keep the root slash), then keep
		 * everything before the last '/'; "." if there is none. Length-aware
		 * (a long leading-directory string must not be truncated). */
		const char *path = ent->path;
		size_t len = ent->pathlen, e = len;
		while (e > 0 && path[e - 1] == '/')
			e--;
		size_t work = (e == 0) ? len : e; /* all-slashes: keep the slash */
		size_t slash = (size_t)-1;
		for (size_t k = 0; k < work; k++)
			if (path[k] == '/')
				slash = k;
		if (slash == (size_t)-1) {
			emit_spec_str(out, s->spec, ".");
		} else {
			char *d = frt_xmalloc(slash + 1);
			memcpy(d, path, slash);
			d[slash] = '\0';
			emit_spec_str(out, s->spec, d);
			free(d);
		}
		return;
	}
	case 'P': {
		const char *cp = "";
		if (ent->depth > 0 && ctx->root_len <= ent->pathlen) {
			cp = ent->path + ctx->root_len;
			if (*cp == '/')
				cp++;
		}
		emit_spec_str(out, s->spec, cp);
		return;
	}
	case 'H':
		emit_spec_str(out, s->spec, ctx->root ? ctx->root : "");
		return;
	case 'd':
		EMIT_SPEC_NUM(out, s->spec, ent->depth);
		return;
	case 'y':
		emit_spec_str(out, s->spec, type_letter(ent->type));
		return;
	case 'i': {
		char buf[32];
		snprintf(buf, sizeof buf, "%llu", (unsigned long long)ent->ino);
		emit_spec_str(out, s->spec, buf);
		return;
	}
	case 'l': {
		char link[4096];
		ssize_t r = -1;
		if (ent->type == FRT_LNK)
			r = readlinkat(ctx->dirfd, ctx->statname, link, sizeof link - 1);
		if (r >= 0) {
			link[r] = '\0';
			emit_spec_str(out, s->spec, link);
		} else {
			emit_spec_str(out, s->spec, "");
		}
		return;
	}
	}

	if (!st) { /* stat failed: emit empty for stat-dependent directives */
		if (s->argtype == ARG_STR)
			emit_spec_str(out, s->spec, "");
		return;
	}

	switch (s->dirc) {
	case 's':
		snprintf(val, sizeof val, "%lld", (long long)st->size);
		emit_spec_str(out, s->spec, val);
		break;
	case 'b':
		snprintf(val, sizeof val, "%lld", (long long)st->blocks);
		emit_spec_str(out, s->spec, val);
		break;
	case 'k':
		snprintf(val, sizeof val, "%lld", (long long)((st->blocks + 1) / 2));
		emit_spec_str(out, s->spec, val);
		break;
	case 'n':
		snprintf(val, sizeof val, "%llu", (unsigned long long)st->nlink);
		emit_spec_str(out, s->spec, val);
		break;
	case 'D':
		snprintf(val, sizeof val, "%llu", (unsigned long long)st->dev);
		emit_spec_str(out, s->spec, val);
		break;
	case 'U':
		snprintf(val, sizeof val, "%llu", (unsigned long long)st->uid);
		emit_spec_str(out, s->spec, val);
		break;
	case 'G':
		snprintf(val, sizeof val, "%llu", (unsigned long long)st->gid);
		emit_spec_str(out, s->spec, val);
		break;
	case 'u': {
		struct passwd *pw = getpwuid(st->uid);
		if (pw)
			emit_spec_str(out, s->spec, pw->pw_name);
		else {
			snprintf(val, sizeof val, "%llu", (unsigned long long)st->uid);
			emit_spec_str(out, s->spec, val);
		}
		break;
	}
	case 'g': {
		struct group *gr = getgrgid(st->gid);
		if (gr)
			emit_spec_str(out, s->spec, gr->gr_name);
		else {
			snprintf(val, sizeof val, "%llu", (unsigned long long)st->gid);
			emit_spec_str(out, s->spec, val);
		}
		break;
	}
	case 'm':
		EMIT_SPEC_NUM(out, s->spec, (unsigned)(st->mode & 07777));
		break;
	case 'M': {
		char ms[16];
		filemodestring(st->mode, ms);
		emit_spec_str(out, s->spec, ms);
		break;
	}
	case 'S': {
		/* find file_sparseness: 512*blocks/size; size 0 -> 1.0 or +/-inf. */
		double sp;
		if (st->size == 0)
			sp = (st->blocks == 0) ? 1.0 : (st->blocks < 0 ? -HUGE_VAL : HUGE_VAL);
		else
			sp = (512.0 * (double)st->blocks) / (double)st->size;
		EMIT_SPEC_NUM(out, s->spec, sp);
		break;
	}
	case 'a':
		ctime_format(st->atime, st->atime_ns, val, sizeof val);
		emit_spec_str(out, s->spec, val);
		break;
	case 'c':
		ctime_format(st->ctime, st->ctime_ns, val, sizeof val);
		emit_spec_str(out, s->spec, val);
		break;
	case 't':
		ctime_format(st->mtime, st->mtime_ns, val, sizeof val);
		emit_spec_str(out, s->spec, val);
		break;
	case 'A':
	case 'C':
	case 'T':
	case 'B': {
		time_t sec;
		long ns;
		if (s->dirc == 'A') { sec = st->atime; ns = st->atime_ns; }
		else if (s->dirc == 'C') { sec = st->ctime; ns = st->ctime_ns; }
		else if (s->dirc == 'B') { sec = st->btime; ns = st->btime_ns; }
		else { sec = st->mtime; ns = st->mtime_ns; }
		if (s->dirc == 'B' && !st->have_btime) {
			emit_spec_str(out, s->spec, "");
			break;
		}
		if (s->aux == '@')
			epoch_ns(sec, ns, val, sizeof val);
		else {
			int want_ns = (s->aux == 'S' || s->aux == 'T' || s->aux == 'X' ||
				       s->aux == '+');
			strftime_ns(s->aux, sec, ns, want_ns, val, sizeof val);
		}
		emit_spec_str(out, s->spec, val);
		break;
	}
	case 'Y': {
		const char *letter;
		if (ent->type != FRT_LNK) {
			letter = mode_type_letter(st->mode);
		} else {
			struct frt_statinfo tgt;
			if (frt_stat_at(ctx->dirfd, ctx->statname, 1, &tgt) == 0) {
				letter = mode_type_letter(tgt.mode);
			} else {
				int e = errno;
				if (e == ENOENT || e == ENOTDIR)
					letter = "N";
				else if (e == ELOOP)
					letter = "L";
				else {
					letter = "?"; /* find also warns, without changing rc */
					frt_diag_errno("", ent->path, e);
				}
			}
		}
		emit_spec_str(out, s->spec, letter);
		break;
	}
	case 'F':
		emit_spec_str(out, s->spec, frt_fstype(ctx->dirfd, ctx->statname));
		break;
	case 'Z':
		emit_spec_str(out, s->spec, ""); /* SELinux: deferred */
		break;
	default:
		break;
	}
}

void fmt_render(const struct fmt *f, struct entry *ent, struct evalctx *ctx, struct dstr *out,
		int out_fd)
{
	for (int i = 0; i < f->nseg; i++) {
		const struct segment *s = &f->segs[i];
		if (s->kind == SEG_PLAIN) {
			dstr_append(out, s->text, s->len);
		} else if (s->kind == SEG_STOP) {
			/* \c : flush output and stop processing this format. */
			out_flush(out, out_fd);
			return;
		} else {
			render_dir(s, ent, ctx, out);
		}
	}
}

bool act_printf(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	struct dstr *out = frt_out_dest(e, ctx);
	int fd = e->u.pf.dest ? e->u.pf.dest->fd : ctx->out_fd;
	fmt_render(e->u.pf.fmt, ent, ctx, out, fd);
	if (e->u.pf.dest)
		frt_outfile_maybe_flush(e->u.pf.dest);
	else
		out_maybe_flush(ctx);
	return true;
}

/* -ls / -fls column widths — static, shared across all -ls actions, grown as
 * files are listed (find/lib/listfile.c defaults). */
static int ls_w_ino = 9, ls_w_blk = 6, ls_w_nlink = 3, ls_w_owner = 8;
static int ls_w_group = 8, ls_w_size = 8;
static int ls_w_major = 3, ls_w_minor = 3; /* device node major,minor columns */
static time_t ls_now;

/* -ls name quoting (find listfile.c print_name_with_quoting): backslash a few
 * specials and space/quote, octal-escape non-printable/non-ASCII bytes. */
static void ls_quote(struct dstr *out, const char *p, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)p[i];
		switch (c) {
		case '\\': dstr_appendz(out, "\\\\"); break;
		case '\n': dstr_appendz(out, "\\n"); break;
		case '\b': dstr_appendz(out, "\\b"); break;
		case '\r': dstr_appendz(out, "\\r"); break;
		case '\t': dstr_appendz(out, "\\t"); break;
		case '\f': dstr_appendz(out, "\\f"); break;
		case ' ':  dstr_appendz(out, "\\ "); break;
		case '"':  dstr_appendz(out, "\\\""); break;
		default:
			if (c > 040 && c < 0177) {
				dstr_appendc(out, (char)c);
			} else {
				char o[8];
				snprintf(o, sizeof o, "\\%03o", c);
				dstr_appendz(out, o);
			}
		}
	}
}

static void ls_num_right(struct dstr *out, int *width, unsigned long long v)
{
	char num[32];
	int len = snprintf(num, sizeof num, "%llu", v);
	char field[80];
	int n = snprintf(field, sizeof field, "%*s", *width, num);
	if (n > 0)
		dstr_append(out, field, (size_t)n);
	if (len > *width)
		*width = len;
}

bool act_ls(const struct expr *e, struct entry *ent, struct evalctx *ctx)
{
	const struct frt_statinfo *st = entry_stat(ent, ctx);
	if (!st)
		return true; /* find lists nothing for an unstattable entry */
	struct dstr *out = frt_out_dest(e, ctx);

	if (ls_now == 0)
		ls_now = time(NULL);

	ls_num_right(out, &ls_w_ino, (unsigned long long)ent->ino);
	dstr_appendc(out, ' ');
	ls_num_right(out, &ls_w_blk, (unsigned long long)((st->blocks + 1) / 2)); /* 1K blocks */
	dstr_appendc(out, ' ');

	char ms[16];
	filemodestring(st->mode, ms);
	dstr_appendz(out, ms);
	dstr_appendc(out, ' '); /* alternate-access-method flag slot */

	ls_num_right(out, &ls_w_nlink, (unsigned long long)st->nlink);
	dstr_appendc(out, ' ');

	char buf[128];
	struct passwd *pw = getpwuid(st->uid);
	if (pw) {
		int len = (int)strlen(pw->pw_name);
		if (len > ls_w_owner)
			ls_w_owner = len;
		snprintf(buf, sizeof buf, "%-*s ", ls_w_owner, pw->pw_name);
	} else {
		snprintf(buf, sizeof buf, "%-8llu ", (unsigned long long)st->uid);
	}
	dstr_appendz(out, buf);

	struct group *gr = getgrgid(st->gid);
	if (gr) {
		int len = (int)strlen(gr->gr_name);
		if (len > ls_w_group)
			ls_w_group = len;
		snprintf(buf, sizeof buf, "%-*s ", ls_w_group, gr->gr_name);
	} else {
		snprintf(buf, sizeof buf, "%-*llu ", ls_w_group, (unsigned long long)st->gid);
	}
	dstr_appendz(out, buf);

	/* device nodes show "major, minor" in the size column (gnulib list_file);
	 * everything else shows the byte size. */
	if (S_ISCHR(st->mode) || S_ISBLK(st->mode)) {
		ls_num_right(out, &ls_w_major, (unsigned long long)major(st->rdev));
		dstr_append(out, ", ", 2);
		ls_num_right(out, &ls_w_minor, (unsigned long long)minor(st->rdev));
	} else {
		ls_num_right(out, &ls_w_size, (unsigned long long)st->size);
	}
	dstr_appendc(out, ' ');

	struct tm *lt = localtime(&st->mtime);
	if (lt) {
		const char *fmt = (ls_now - 6 * 30 * 24 * 60 * 60 <= st->mtime &&
				   st->mtime <= ls_now + 60 * 60)
					  ? "%b %e %H:%M"
					  : "%b %e  %Y";
		char db[128];
		strftime(db, sizeof db, fmt, lt);
		dstr_appendz(out, db);
		dstr_appendc(out, ' ');
	}

	ls_quote(out, ent->path, ent->pathlen);

	if (ent->type == FRT_LNK) {
		char link[4096];
		ssize_t r = readlinkat(ctx->dirfd, ctx->statname, link, sizeof link - 1);
		if (r >= 0) {
			dstr_appendz(out, " -> ");
			ls_quote(out, link, (size_t)r);
		}
	}
	dstr_appendc(out, '\n');
	if (e->u.pf.dest)
		frt_outfile_maybe_flush(e->u.pf.dest);
	else
		out_maybe_flush(ctx);
	return true;
}
