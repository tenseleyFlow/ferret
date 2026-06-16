#include "parse.h"
#include "pred.h"
#include "action.h"
#include "diag.h"
#include "exec.h"
#include "fmt.h"
#include "glob.h"
#include "outfile.h"
#include "xregex.h"
#include "sys/xstat.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DAYSECS 86400

/* bfs cost constants (audit 03, decided). */
#define COST_FAST 40.0f
#define COST_FNMATCH 400.0f
#define COST_STAT 1000.0f
#define COST_PRINT 20000.0f

/* Bound on '(' nesting and on the length of a single AND/OR/comma chain. The
 * parser, the optimizer (annotate/flatten), and eval_expr all recurse to the
 * expression-tree depth; a pathological input (tens of thousands of nested
 * parens or chained predicates) would otherwise overflow the C stack and
 * SIGSEGV. find handles such input (slowly); ferret rejects it cleanly past
 * this cap, well above any real expression. Recorded in deviations.md. */
#define FRT_EXPR_DEPTH_MAX 4000

/* ---- parser state ---------------------------------------------------------- */

struct pstate {
	char **argv;
	int argc;
	int i;
	struct arena *arena;
	struct options *opts;     /* positional options write here */
	struct outfile **outfiles; /* -f* destination registry */
	const char *error;     /* set on failure: complete message body */
	int paren_depth;       /* '(' nesting, capped to bound parser C-recursion */
	bool has_action;       /* any action seen => suppress implicit -print */
	/* time origin (find: start_time; cur_day_start defaults to start-DAYSECS,
	 * -daystart floors it to local midnight; positional — affects later tests). */
	struct timespec start_time;
	struct timespec cur_day_start;
	int full_days; /* -daystart already applied */
};

static char *cur(struct pstate *ps)
{
	return ps->i < ps->argc ? ps->argv[ps->i] : NULL;
}

static void advance(struct pstate *ps)
{
	if (ps->i < ps->argc)
		ps->i++;
}

static struct expr *new_node(struct pstate *ps, enum expr_kind kind)
{
	struct expr *e = arena_alloc(ps->arena, sizeof *e);
	memset(e, 0, sizeof *e);
	e->kind = kind;
	e->pure = true;
	return e;
}

static struct expr *mk_unop(struct pstate *ps, enum expr_kind kind, struct expr *child)
{
	struct expr *e = new_node(ps, kind);
	e->lhs = child;
	e->pure = child->pure;
	return e;
}

static struct expr *mk_binop(struct pstate *ps, enum expr_kind kind, struct expr *l,
			     struct expr *r)
{
	struct expr *e = new_node(ps, kind);
	e->lhs = l;
	e->rhs = r;
	e->pure = l->pure && r->pure;
	return e;
}

/* ---- predicate table ------------------------------------------------------- */

static bool tok_is(const char *t, const char *a, const char *b)
{
	return t && (strcmp(t, a) == 0 || (b && strcmp(t, b) == 0));
}

/* Format a parse error into the arena and stash it on ps. find builds several
 * of its messages inline with the predicate name, so they can't go through a
 * fixed key table. */
static void set_errorf(struct pstate *ps, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		ps->error = "invalid expression";
		return;
	}
	char *buf = arena_alloc(ps->arena, (size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(buf, (size_t)n + 1, fmt, ap);
	va_end(ap);
	ps->error = buf;
}

/* find quotes user-supplied values with the locale's quoting style: ASCII
 * 'apostrophes' in C, ‘typographic’ in UTF-8. These return the open/close
 * marks so set_errorf can splice them in as %s. */
static const char *q_open(void) { return frt_diag_utf8() ? "\xe2\x80\x98" : "'"; }
static const char *q_close(void) { return frt_diag_utf8() ? "\xe2\x80\x99" : "'"; }

/* Stat a reference file for -samefile/-newer/-newerXY. Under -L/-H, a dangling
 * symlink reference (deref fails with ENOENT) falls back to lstat and uses the
 * link itself, like find, instead of erroring. 0 / -1 with the original
 * (follow) errno preserved for the diagnostic. */
static int stat_reference(int follow, const char *arg, struct frt_statinfo *si)
{
	if (frt_stat_at(AT_FDCWD, arg, follow, si) == 0)
		return 0;
	int e = errno;
	if (follow && e == ENOENT && frt_stat_at(AT_FDCWD, arg, 0, si) == 0)
		return 0;
	errno = e;
	return -1;
}

/* find's -regextype error lists every accepted dialect, each locale-quoted. */
static void set_regextype_error(struct pstate *ps, const char *arg)
{
	static const char *const types[] = {
		"findutils-default", "ed", "emacs", "gnu-awk", "grep", "posix-awk",
		"awk", "posix-basic", "posix-egrep", "egrep", "posix-extended",
		"posix-minimal-basic", "sed",
	};
	const char *oq = q_open(), *cq = q_close();
	char list[768];
	size_t n = 0;
	for (size_t i = 0; i < sizeof types / sizeof *types && n < sizeof list; i++)
		n += (size_t)snprintf(list + n, sizeof list - n, "%s%s%s%s",
				      i ? ", " : "", oq, types[i], cq);
	set_errorf(ps, "Unknown regular expression type %s%s%s; valid types are %s.",
		   oq, arg, cq, list);
}

/* Parse a -type/-xtype comma list, matching find's grammar and its five
 * distinct diagnostics (insert_type). `pred` is "-type" or "-xtype" and is
 * spliced into the message. On error sets ps->error and returns -1. */
static int parse_type_list(struct pstate *ps, const char *pred, const char *arg,
			   unsigned *mask_out)
{
	if (!*arg) {
		set_errorf(ps, "Arguments to %s should contain at least one letter", pred);
		return -1;
	}
	unsigned mask = 0;
	const char *p = arg;
	for (;;) {
		unsigned bit;
		switch (*p) {
		case 'b': bit = FRT_BLK; break;
		case 'c': bit = FRT_CHR; break;
		case 'd': bit = FRT_DIR; break;
		case 'f': bit = FRT_REG; break;
		case 'l': bit = FRT_LNK; break;
		case 'p': bit = FRT_FIFO; break;
		case 's': bit = FRT_SOCK; break;
		case 'D': /* Solaris door: a known letter, unsupported here */
			set_errorf(ps, "%s %c is not supported because Solaris doors "
				       "are not supported on the platform find was "
				       "compiled on.", pred, *p);
			return -1;
		default:
			set_errorf(ps, "Unknown argument to %s: %c", pred, *p);
			return -1;
		}
		if (mask & (1u << bit)) {
			set_errorf(ps, "Duplicate file type '%c' in the argument list to %s.",
				   *p, pred);
			return -1;
		}
		mask |= 1u << bit;
		p++;
		if (!*p)
			break;
		if (*p != ',') {
			set_errorf(ps, "Must separate multiple arguments to %s using: ','", pred);
			return -1;
		}
		p++;
		if (!*p) {
			set_errorf(ps, "Last file type in list argument to %s is missing, "
				       "i.e., list is ending on: ','", pred);
			return -1;
		}
	}
	*mask_out = mask;
	return 0;
}

/* Parse a non-negative decimal integer (for -maxdepth/-mindepth). -1 on error. */
static int parse_nonneg(const char *s)
{
	if (!s || !s[0])
		return -1;
	int v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		v = v * 10 + (*p - '0');
		if (v > 1000000000)
			return -1;
	}
	return v;
}

/* Parse +N / -N / N into a comparison kind and value. Returns 0 / -1. find
 * parses the magnitude as uintmax_t (xstrtoumax), so the limit is ULLONG_MAX;
 * anything larger is rejected, not silently wrapped. */
static int parse_num_arg(const char *s, int *kind, unsigned long long *val)
{
	if (!s)
		return -1;
	*kind = COMP_EQ;
	if (*s == '+') {
		*kind = COMP_GT;
		s++;
	} else if (*s == '-') {
		*kind = COMP_LT;
		s++;
	}
	/* find parses the magnitude with xstrtoumax, which (via strtoumax) skips
	 * leading whitespace and takes an optional sign. strtoull matches that;
	 * reject empty, trailing junk, and overflow. */
	errno = 0;
	char *end;
	unsigned long long v = strtoull(s, &end, 10);
	if (end == s || *end != '\0' || errno == ERANGE)
		return -1;
	*val = v;
	return 0;
}

/* Parse a numeric uid/gid: base-10, no trailing junk, within [0, maxval].
 * Mirrors find's xstrtoumax + UID_T_MAX/GID_T_MAX check. 0 / -1. */
static int parse_id(const char *s, unsigned long long maxval, unsigned long long *out)
{
	errno = 0;
	char *end;
	unsigned long long v = strtoull(s, &end, 10);
	if (end == s || *end != '\0' || errno == ERANGE || v > maxval)
		return -1;
	*out = v;
	return 0;
}

/* Parse a -size argument: [+-]N[bcwkMG]. Returns 0, -1 (bad number), -2 (bad
 * suffix). On bad suffix, *suffix is the offending char. find takes the unit
 * from the LAST character, then parses everything before it as the number, so
 * "100baz" is rejected on 'z' (not 'a') and "abc" is a bad number, not a bad
 * suffix. */
static int parse_size_arg(const char *s, int *kind, unsigned long long *val, long long *unit, char *suffix)
{
	size_t len = s ? strlen(s) : 0;
	if (len == 0)
		return -1;
	long long u;
	size_t numlen = len - 1; /* number part when a suffix is present */
	switch (s[len - 1]) {
	case 'b': u = 512; break;
	case 'c': u = 1; break;
	case 'w': u = 2; break;
	case 'k': u = 1024; break;
	case 'M': u = 1024LL * 1024; break;
	case 'G': u = 1024LL * 1024 * 1024; break;
	default:
		if (s[len - 1] >= '0' && s[len - 1] <= '9') {
			u = 512;      /* no suffix: default 'b' = 512-byte blocks */
			numlen = len;
		} else {
			*suffix = s[len - 1];
			return -2;
		}
	}
	*kind = COMP_EQ;
	const char *p = s;
	const char *pend = s + numlen;
	if (p < pend && (*p == '+' || *p == '-')) {
		*kind = (*p == '+') ? COMP_GT : COMP_LT;
		p++;
	}
	while (p < pend && isspace((unsigned char)*p)) /* xstrtoumax skips leading ws */
		p++;
	if (p == pend)
		return -1; /* nothing but a sign/whitespace */
	unsigned long long v = 0;
	for (; p < pend; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		unsigned d = (unsigned)(*p - '0');
		if (v > (ULLONG_MAX - d) / 10)
			return -1; /* overflow: find rejects via get_num */
		v = v * 10 + d;
	}
	*val = v;
	*unit = u;
	return 0;
}

/* Parse an octal or symbolic mode (base 0, as -perm requires). `x_as_exec`
 * controls whether the conditional `X` contributes the execute bit (find
 * resolves X per file; the caller parses once each way and picks at match
 * time). Returns 0 / -1. Symbolic grammar matches chmod / gnulib modechange:
 *   [ugoa]* ( [-+=] [rwxXstugo]* )+  (',' clause)*
 * with `u`/`g`/`o` in the perms position copying that class's current bits. */
static int parse_mode_str(const char *s, int x_as_exec, unsigned *out)
{
	if (!s || !*s)
		return -1;
	int octal = 1;
	for (const char *p = s; *p; p++)
		if (*p < '0' || *p > '7') {
			octal = 0;
			break;
		}
	if (octal) {
		unsigned v = 0;
		for (const char *p = s; *p; p++) {
			v = v * 8u + (unsigned)(*p - '0');
			if (v > 07777u)
				return -1; /* find rejects modes >= 010000 */
		}
		*out = v;
		return 0;
	}

	unsigned mode = 0;
	const char *p = s;
	for (;;) {
		unsigned who = 0;
		int who_set = 0;
		for (; *p; p++) {
			if (*p == 'u')
				who |= 0700u, who_set = 1;
			else if (*p == 'g')
				who |= 0070u, who_set = 1;
			else if (*p == 'o')
				who |= 0007u, who_set = 1;
			else if (*p == 'a')
				who |= 0777u, who_set = 1;
			else
				break;
		}
		unsigned eff_who = who_set ? who : 0777u; /* bare op = all classes */
		if (*p != '+' && *p != '-' && *p != '=')
			return -1; /* a clause needs at least one operator */
		/* one or more [op][perms] sub-clauses sharing this who */
		do {
			char op = *p++;
			unsigned rwx = 0, sbit = 0, tbit = 0;
			for (; *p && *p != ',' && *p != '+' && *p != '-' && *p != '='; p++) {
				switch (*p) {
				case 'r': rwx |= 4u; break;
				case 'w': rwx |= 2u; break;
				case 'x': rwx |= 1u; break;
				case 'X': if (x_as_exec) rwx |= 1u; break;
				case 's': sbit = 1; break;
				case 't': tbit = 1; break;
				case 'u': rwx |= (mode >> 6) & 7u; break; /* copy from user */
				case 'g': rwx |= (mode >> 3) & 7u; break; /* copy from group */
				case 'o': rwx |= mode & 7u; break;        /* copy from other */
				default:  return -1;
				}
			}
			unsigned value = 0;
			if (eff_who & 0700u)
				value |= rwx << 6;
			if (eff_who & 0070u)
				value |= rwx << 3;
			if (eff_who & 0007u)
				value |= rwx;
			if (sbit) {
				if (eff_who & 0700u)
					value |= 04000u;
				if (eff_who & 0070u)
					value |= 02000u;
			}
			if (tbit)
				value |= 01000u;

			if (op == '+')
				mode |= value;
			else if (op == '-')
				mode &= ~value;
			else { /* '=' clears the affected who triples + their special bits */
				unsigned clear = 0;
				if (eff_who & 0700u)
					clear |= 0700u | 04000u;
				if (eff_who & 0070u)
					clear |= 0070u | 02000u;
				if (eff_who & 0007u)
					clear |= 0007u | 01000u;
				mode = (mode & ~clear) | value;
			}
		} while (*p == '+' || *p == '-' || *p == '=');
		if (*p == '\0')
			break;
		if (*p != ',')
			return -1;
		p++;
		if (*p == '\0')
			return -1; /* trailing comma rejected */
	}
	*out = mode & 07777u;
	return 0;
}

/* -daystart: floor cur_day_start to local midnight today (find/parser.c). Only
 * the first occurrence has effect; positional (affects later time tests only). */
static void apply_daystart(struct pstate *ps)
{
	if (ps->full_days)
		return;
	ps->cur_day_start.tv_sec += DAYSECS; /* from start-DAYSECS back to start */
	ps->cur_day_start.tv_nsec = 0;
	time_t t = (time_t)ps->cur_day_start.tv_sec;
	struct tm *lt = localtime(&t);
	if (lt)
		ps->cur_day_start.tv_sec -=
			lt->tm_sec + lt->tm_min * 60 + lt->tm_hour * 3600;
	else
		ps->cur_day_start.tv_sec -= ps->cur_day_start.tv_sec % DAYSECS;
	ps->full_days = 1;
}

/* Build a relative time predicate (-atime/-mtime/... and -amin/-mmin/...).
 * `unit` is DAYSECS or 60. Mirrors find's parse_time / do_parse_xmin +
 * get_relative_timestamp exactly (audit 01, sense inversion, -N day fudge). */
static struct expr *build_time_pred(struct pstate *ps, struct expr *e, int field, int unit,
				    const char *pname, const char *arg)
{
	if (!arg) {
		set_errorf(ps, "missing argument to `%s'", pname);
		return NULL;
	}
	struct timespec origin = ps->cur_day_start;
	const char *p = arg;
	int comp = COMP_EQ;
	if (*p == '+') {
		comp = COMP_GT;
		p++;
	} else if (*p == '-') {
		comp = COMP_LT;
		p++;
	}
	if (unit == DAYSECS) {
		if (comp == COMP_LT) /* -N: end-of-day fudge */
			origin.tv_sec += DAYSECS - 1;
	} else {
		origin.tv_sec += DAYSECS; /* minutes measure from "now" */
	}

	char *end;
	double offset = strtod(p, &end);
	if (end == p || *end != '\0') { /* empty / non-numeric / trailing junk */
		set_errorf(ps, "invalid argument `%s' to `%s'", arg, pname);
		return NULL;
	}
	if (offset != offset) { /* NaN (its own message; magnitude is sign-stripped) */
		set_errorf(ps, "invalid not-a-number argument: `%s'", p);
		return NULL;
	}
	int kind = comp == COMP_LT ? COMP_GT : comp == COMP_GT ? COMP_LT : COMP_EQ;

	/* split offset*unit into whole seconds + fractional ns. find accepts inf
	 * and values far beyond time_t (the reference just lands at -/+infinity, so
	 * the comparison matches none/all). Saturate to avoid UB casting an
	 * out-of-range double to long long. Avoids libm. */
	double total = offset * (double)unit;
	long long rsec;
	long rnsec = 0;
	if (total >= 9.0e18) { /* reference effectively at -infinity */
		rsec = LLONG_MIN;
	} else if (total <= -9.0e18) { /* reference effectively at +infinity */
		rsec = LLONG_MAX;
	} else {
		long long secs_d = (long long)total;
		double frac = total - (double)secs_d;
		long nanosec = (long)(frac * 1.0e9);
		rsec = (long long)origin.tv_sec - secs_d;
		rnsec = (long)origin.tv_nsec - nanosec;
		if (rnsec < 0) {
			rnsec += 1000000000L;
			rsec -= 1;
		}
	}

	e->pred = PRED_TIME;
	e->eval = pred_time;
	e->needs_stat = true;
	e->u.time.kind = kind;
	e->u.time.field = field;
	e->u.time.ref_sec = rsec;
	e->u.time.ref_nsec = rnsec;
	e->u.time.window = unit;
	e->cost = COST_STAT;
	e->prob = 0.5f;
	return e;
}

/* Parse a date for -newerXt / -newermt. Subset of find's parse_datetime:
 * @EPOCH, "YYYY-MM-DD[ HH:MM:SS]" in local time. Returns 0/-1. */
static int parse_datetime_basic(const char *s, long long *sec, long *nsec)
{
	*nsec = 0;
	if (s[0] == '@') {
		char *end;
		long long v = strtoll(s + 1, &end, 10);
		if (end == s + 1 || *end != '\0')
			return -1;
		*sec = v;
		return 0;
	}
	struct tm tm;
	memset(&tm, 0, sizeof tm);
	tm.tm_isdst = -1;
	char *r = strptime(s, "%Y-%m-%d %H:%M:%S", &tm);
	if (!r || *r) {
		memset(&tm, 0, sizeof tm);
		tm.tm_isdst = -1;
		r = strptime(s, "%Y-%m-%d", &tm);
		if (!r || *r)
			return -1;
	}
	time_t t = mktime(&tm);
	if (t == (time_t)-1)
		return -1;
	*sec = t;
	return 0;
}

/* Parse -exec/-execdir/-ok/-okdir: collect the command template up to ';' or
 * '+', validate {} rules (audit 02). execdir/ok control the dir/prompt behavior;
 * '+' is allowed only for -exec/-execdir (not -ok/-okdir). */
static struct expr *parse_exec(struct pstate *ps, struct expr *e, int execdir, int ok)
{
	const char *pname = ok ? (execdir ? "-okdir" : "-ok")
			       : (execdir ? "-execdir" : "-exec");
	int allow_plus = !ok;
	int start = ps->i;
	int term = 0; /* 1=';', 2='+' */
	int end = -1;
	int prev_braces = 0;
	for (int k = ps->i; k < ps->argc; k++) {
		const char *t = ps->argv[k];
		/* '+' terminates only when the previous arg contained '{}' (find tests
		 * with mbsstr, i.e. '{}' anywhere in the token, not just a bare "{}"). */
		if (allow_plus && t[0] == '+' && t[1] == '\0' && prev_braces) {
			term = 2;
			end = k;
			break;
		}
		if (strcmp(t, ";") == 0) {
			term = 1;
			end = k;
			break;
		}
		prev_braces = (strstr(t, "{}") != NULL);
	}
	if (term == 0) {
		/* ran off the end with no ';' or valid '+' terminator (tree.c) */
		set_errorf(ps, "missing argument to `%s'", pname);
		return NULL;
	}
	int ntmpl = end - start;
	if (ntmpl == 0) {
		/* the terminator is the first token: empty command */
		set_errorf(ps, "invalid argument `%s' to `%s'", ps->argv[end], pname);
		return NULL;
	}
	char **tmpl = arena_alloc(ps->arena, (size_t)ntmpl * sizeof(char *));
	for (int k = 0; k < ntmpl; k++)
		tmpl[k] = arena_strdup(ps->arena, ps->argv[start + k]);
	ps->i = end + 1;

	/* NOTE: find 4.10.0 documents that {} is forbidden in the -execdir/-okdir
	 * utility name, but the check (parser.c: `0 == end`) is dead code and never
	 * fires, so find actually substitutes it. Parity-first: match find and allow
	 * it (recorded in .docs/deviations.md). */
	if (term == 2) {
		const char *suffix = execdir ? "dir" : "";
		int brace_count = 0;
		const char *brace_arg = NULL;
		for (int k = 0; k < ntmpl; k++) {
			if (strstr(tmpl[k], "{}")) {
				brace_count++;
				brace_arg = tmpl[k];
			}
		}
		if (brace_count > 1) {
			set_errorf(ps, "Only one instance of {} is supported with -exec%s ... +",
				   suffix);
			return NULL;
		}
		if (brace_arg && strlen(brace_arg) != 2) {
			/* find quotes these three with the locale's quoting style. */
			const char *oq = frt_diag_utf8() ? "\xe2\x80\x98" : "'";
			const char *cq = frt_diag_utf8() ? "\xe2\x80\x99" : "'";
			set_errorf(ps, "In %s-exec%s ... {} +%s the %s{}%s must appear by "
				       "itself, but you specified %s%s%s",
				   oq, suffix, cq, oq, cq, oq, brace_arg, cq);
			return NULL;
		}
	}

	e->pred = ACT_EXEC;
	e->eval = act_exec;
	e->pure = false;
	e->u.exec.tmpl = tmpl;
	e->u.exec.ntmpl = ntmpl;
	e->u.exec.multiple = (term == 2);
	e->u.exec.execdir = execdir;
	e->u.exec.ok = ok;
	e->u.exec.batch = NULL;
	e->cost = (term == 2) ? 100.0f : 100000.0f; /* + amortizes; ; forks per file */
	e->prob = 1.0f;
	ps->has_action = true;
	frt_exec_init(e);
	return e;
}

/* A positional option (-depth, -xdev, ...) evaluates to a no-op true; its effect
 * is recorded in ps->opts at parse time. */
static struct expr *mk_option_leaf(struct pstate *ps)
{
	struct expr *e = new_node(ps, EXPR_LEAF);
	e->pred = PRED_OPTION;
	e->eval = pred_true;
	e->cost = COST_FAST;
	e->prob = 1.0f;
	return e;
}

/* Build a leaf for predicate token `name`, consuming its arguments. Returns NULL
 * and sets ps->error on failure. Assumes ps->i points at the predicate token. */
static struct expr *parse_predicate(struct pstate *ps)
{
	const char *name = cur(ps);
	advance(ps);

	struct expr *e = new_node(ps, EXPR_LEAF);

	if (strcmp(name, "-name") == 0 || strcmp(name, "-iname") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		e->pred = name[1] == 'i' ? PRED_INAME : PRED_NAME;
		e->eval = pred_name;
		e->u.name.pattern = arena_strdup(ps->arena, arg);
		e->u.name.glob_flags = (name[1] == 'i') ? FRT_GLOB_CASEFOLD : 0;
		e->u.name.glob_kind = (unsigned char)frt_glob_classify(
			e->u.name.pattern, e->u.name.glob_flags, &e->u.name.glob_litlen);
		e->cost = COST_FNMATCH;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-type") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		unsigned mask;
		if (parse_type_list(ps, "-type", arg, &mask) != 0)
			return NULL;
		e->pred = PRED_TYPE;
		e->eval = pred_type;
		e->u.type.mask = mask;
		e->cost = COST_FAST;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-empty") == 0) {
		e->pred = PRED_EMPTY;
		e->eval = pred_empty;
		e->needs_stat = true;
		e->cost = 2 * COST_STAT;
		e->prob = 0.01f;
		return e;
	}
	if (strcmp(name, "-path") == 0 || strcmp(name, "-wholename") == 0 ||
	    strcmp(name, "-ipath") == 0 || strcmp(name, "-iwholename") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		e->pred = PRED_PATH;
		e->eval = pred_path;
		e->u.name.pattern = arena_strdup(ps->arena, arg);
		e->u.name.glob_flags = name[1] == 'i' ? FRT_GLOB_CASEFOLD : 0;
		e->u.name.glob_kind = (unsigned char)frt_glob_classify(
			e->u.name.pattern, e->u.name.glob_flags, &e->u.name.glob_litlen);
		e->cost = COST_FNMATCH;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-lname") == 0 || strcmp(name, "-ilname") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		e->pred = PRED_LNAME;
		e->eval = pred_lname;
		e->u.name.pattern = arena_strdup(ps->arena, arg);
		e->u.name.glob_flags = name[1] == 'i' ? FRT_GLOB_CASEFOLD : 0;
		e->u.name.glob_kind = (unsigned char)frt_glob_classify(
			e->u.name.pattern, e->u.name.glob_flags, &e->u.name.glob_litlen);
		e->cost = COST_FNMATCH;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-xtype") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		unsigned mask;
		if (parse_type_list(ps, "-xtype", arg, &mask) != 0)
			return NULL;
		advance(ps);
		e->pred = PRED_XTYPE;
		e->eval = pred_xtype;
		e->u.type.mask = mask;
		e->needs_stat = true;
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-fstype") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		e->pred = PRED_FSTYPE;
		e->eval = pred_fstype;
		e->u.fstype = arena_strdup(ps->arena, arg);
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-regex") == 0 || strcmp(name, "-iregex") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		const char *rerr = NULL;
		struct frt_regex *re = frt_regex_compile(
			arg, ps->opts->regextype, name[1] == 'i', ps->arena, &rerr);
		if (!re) {
			/* find's wrapper; the reason text comes from the system regex
			 * engine and may read differently than gnulib's (deviations.md). */
			set_errorf(ps, "failed to compile regular expression '%s': %s",
				   arg, rerr ? rerr : "Invalid regular expression");
			return NULL;
		}
		e->pred = PRED_REGEX;
		e->eval = pred_regex;
		e->u.regex = re;
		e->cost = COST_FNMATCH;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-regextype") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		int rt = frt_regextype_from_name(arg);
		if (rt < 0) {
			set_regextype_error(ps, arg);
			return NULL;
		}
		advance(ps);
		ps->opts->regextype = rt;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-size") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		int kind;
		unsigned long long val;
		long long unit;
		if (arg[0] == '\0') { /* find has a specific message for the empty arg */
			set_errorf(ps, "invalid null argument to -size");
			return NULL;
		}
		char bad = 0;
		int rc = parse_size_arg(arg, &kind, &val, &unit, &bad);
		if (rc == -2) {
			set_errorf(ps, "invalid -size type `%c'", bad);
			return NULL;
		}
		if (rc != 0) {
			set_errorf(ps, "Invalid argument `%s' to -size", arg);
			return NULL;
		}
		advance(ps);
		e->pred = PRED_SIZE;
		e->eval = pred_size;
		e->needs_stat = true;
		e->u.size.kind = kind;
		e->u.size.val = val;
		e->u.size.unit = unit;
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-links") == 0 || strcmp(name, "-inum") == 0 ||
	    strcmp(name, "-uid") == 0 || strcmp(name, "-gid") == 0) {
		const char *arg = cur(ps);
		int kind;
		unsigned long long val;
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		if (parse_num_arg(arg, &kind, &val) != 0) {
			set_errorf(ps, "non-numeric argument to %s: %s%s%s", name,
				   q_open(), arg, q_close());
			return NULL;
		}
		advance(ps);
		e->eval = name[1] == 'l'   ? pred_links
			  : name[1] == 'i' ? pred_inum
			  : name[1] == 'u' ? pred_uid
					   : pred_gid;
		e->pred = name[1] == 'l'   ? PRED_LINKS
			  : name[1] == 'i' ? PRED_INUM
			  : name[1] == 'u' ? PRED_UID
					   : PRED_GID;
		e->needs_stat = true;
		e->u.num.kind = kind;
		e->u.num.val = val;
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-user") == 0 || strcmp(name, "-group") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		int is_user = name[1] == 'u';
		unsigned long long id;
		/* Name first; if unknown, fall back to a numeric id bounded by the
		 * type's max (find's get_uid/get_gid via xstrtoumax). */
		if (is_user) {
			struct passwd *pw = getpwnam(arg);
			if (pw)
				id = pw->pw_uid;
			else if (parse_id(arg, (unsigned long long)(uid_t)-1, &id) != 0) {
				set_errorf(ps, "invalid user name or UID argument to %s: %s%s%s",
					   name, q_open(), arg, q_close());
				return NULL;
			}
		} else {
			struct group *gr = getgrnam(arg);
			if (gr)
				id = gr->gr_gid;
			else if (parse_id(arg, (unsigned long long)(gid_t)-1, &id) != 0) {
				set_errorf(ps, "invalid group name or GID argument to %s: %s%s%s",
					   name, q_open(), arg, q_close());
				return NULL;
			}
		}
		e->eval = is_user ? pred_uid : pred_gid;
		e->pred = is_user ? PRED_UID : PRED_GID;
		e->needs_stat = true;
		e->u.num.kind = COMP_EQ;
		e->u.num.val = id;
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-nouser") == 0 || strcmp(name, "-nogroup") == 0) {
		int is_user = name[3] == 'u';
		e->eval = is_user ? pred_nouser : pred_nogroup;
		e->pred = is_user ? PRED_NOUSER : PRED_NOGROUP;
		e->needs_stat = true;
		e->cost = COST_STAT;
		e->prob = 0.01f;
		return e;
	}
	if (strcmp(name, "-samefile") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		struct frt_statinfo si;
		int follow = ps->opts->follow != 0;
		if (stat_reference(follow, arg, &si) != 0) {
			set_errorf(ps, "%s%s%s: %s", q_open(), arg, q_close(), strerror(errno));
			return NULL;
		}
		e->pred = PRED_SAMEFILE;
		e->eval = pred_samefile;
		e->needs_stat = true;
		e->u.samefile.dev = si.dev;
		e->u.samefile.ino = si.ino;
		e->cost = COST_STAT;
		e->prob = 0.01f;
		return e;
	}
	if (strcmp(name, "-perm") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		int match = PERM_EXACT;
		const char *m = arg;
		if (*m == '-') {
			match = PERM_ALL;
			m++;
		} else if (*m == '/') {
			match = PERM_ANY;
			m++;
		}
		/* find 4.10.0 has no '+' prefix: '+SYMBOLIC' (e.g. +rw) is an ordinary
		 * exact mode parsed whole, while '+OCTAL' is rejected. The old GNU
		 * '+OCTAL' = '/OCTAL' extension was removed because it clashed with
		 * chmod's reading of the same string (parser.c). */
		if (arg[0] == '+' && arg[1] >= '0' && arg[1] <= '7') {
			set_errorf(ps, "invalid mode %s%s%s", q_open(), arg, q_close());
			return NULL;
		}
		unsigned mode, mode_x;
		/* Parse twice: once with the conditional X off, once on. pred_perm
		 * picks per file (X = execute iff dir or already executable). */
		if (parse_mode_str(m, 0, &mode) != 0 || parse_mode_str(m, 1, &mode_x) != 0) {
			set_errorf(ps, "invalid mode %s%s%s", q_open(), arg, q_close());
			return NULL;
		}
		/* find warns (unconditionally, even with -nowarn) when a '/' mask
		 * resolves to 0, since -perm /000 now matches everything. */
		if (match == PERM_ANY && mode == 0 && mode_x == 0)
			fprintf(stderr,
				"ferret: warning: you have specified a mode pattern %s (which is "
				"equivalent to /000). The meaning of -perm /000 has now been "
				"changed to be consistent with -perm -000; that is, while it used "
				"to match no files, it now matches all files.\n",
				arg);
		e->pred = PRED_PERM;
		e->eval = pred_perm;
		e->needs_stat = true;
		e->u.perm.match = match;
		e->u.perm.mode = mode;
		e->u.perm.mode_x = mode_x;
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-readable") == 0 || strcmp(name, "-writable") == 0 ||
	    strcmp(name, "-executable") == 0) {
		e->pred = PRED_ACCESS;
		e->eval = pred_access;
		e->needs_stat = true;
		e->u.access.amode = name[1] == 'r' ? R_OK : name[1] == 'w' ? W_OK : X_OK;
		e->cost = COST_STAT;
		e->prob = name[1] == 'r' ? 0.99f : name[1] == 'w' ? 0.8f : 0.2f;
		return e;
	}
	if (strcmp(name, "-atime") == 0 || strcmp(name, "-ctime") == 0 ||
	    strcmp(name, "-mtime") == 0) {
		int field = name[1] == 'a' ? TF_ATIME : name[1] == 'c' ? TF_CTIME : TF_MTIME;
		const char *arg = cur(ps);
		struct expr *r = build_time_pred(ps, e, field, DAYSECS, name, arg);
		if (r)
			advance(ps);
		return r;
	}
	if (strcmp(name, "-amin") == 0 || strcmp(name, "-cmin") == 0 ||
	    strcmp(name, "-mmin") == 0) {
		int field = name[1] == 'a' ? TF_ATIME : name[1] == 'c' ? TF_CTIME : TF_MTIME;
		const char *arg = cur(ps);
		struct expr *r = build_time_pred(ps, e, field, 60, name, arg);
		if (r)
			advance(ps);
		return r;
	}
	if (strcmp(name, "-newer") == 0 || strcmp(name, "-anewer") == 0 ||
	    strcmp(name, "-cnewer") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		struct frt_statinfo si;
		if (stat_reference(ps->opts->follow != 0, arg, &si) != 0) {
			set_errorf(ps, "%s%s%s: %s", q_open(), arg, q_close(), strerror(errno));
			return NULL;
		}
		int field = name[1] == 'n' ? TF_MTIME : name[1] == 'a' ? TF_ATIME : TF_CTIME;
		e->pred = PRED_TIME;
		e->eval = pred_time;
		e->needs_stat = true;
		e->u.time.kind = COMP_GT;
		e->u.time.field = field;
		e->u.time.ref_sec = si.mtime;
		e->u.time.ref_nsec = si.mtime_ns;
		e->u.time.window = 0;
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strncmp(name, "-newer", 6) == 0 && strlen(name) == 8) {
		char X = name[6], Y = name[7];
		int xf = X == 'a' ? TF_ATIME : X == 'c' ? TF_CTIME : X == 'm' ? TF_MTIME
			 : X == 'B' ? TF_BTIME : -1;
		if (xf < 0 || strchr("aBcmt", Y) == NULL) {
			set_errorf(ps, "invalid predicate `%s'", name);
			return NULL;
		}
		const char *arg = cur(ps);
		if (!arg) {
			/* the 8-char -newerXY family uses a different missing-arg wording
			 * than the -newer/-anewer/-cnewer "missing argument to" form. */
			set_errorf(ps, "The %s%s%s test needs an argument", q_open(), name,
				   q_close());
			return NULL;
		}
		advance(ps);
		long long rsec;
		long rnsec = 0;
		if (Y == 't') {
			if (parse_datetime_basic(arg, &rsec, &rnsec) != 0) {
				set_errorf(ps, "I cannot figure out how to interpret "
					       "%s%s%s as a date or time",
					   q_open(), arg, q_close());
				return NULL;
			}
		} else {
			struct frt_statinfo si;
			if (stat_reference(ps->opts->follow != 0, arg, &si) != 0) {
				set_errorf(ps, "%s%s%s: %s", q_open(), arg, q_close(),
					   strerror(errno));
				return NULL;
			}
			switch (Y) {
			case 'a': rsec = si.atime; rnsec = si.atime_ns; break;
			case 'c': rsec = si.ctime; rnsec = si.ctime_ns; break;
			case 'm': rsec = si.mtime; rnsec = si.mtime_ns; break;
			default: /* 'B' */
				if (!si.have_btime) {
					set_errorf(ps, "%s%s%s: birth time is not available "
						       "for this file", q_open(), arg, q_close());
					return NULL;
				}
				rsec = si.btime;
				rnsec = si.btime_ns;
				break;
			}
		}
		e->pred = PRED_TIME;
		e->eval = pred_time;
		e->needs_stat = true;
		e->u.time.kind = COMP_GT;
		e->u.time.field = xf;
		e->u.time.ref_sec = rsec;
		e->u.time.ref_nsec = rnsec;
		e->u.time.window = 0;
		e->cost = COST_STAT;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-true") == 0) {
		e->pred = PRED_TRUE;
		e->eval = pred_true;
		e->cost = COST_FAST;
		e->prob = 1.0f;
		return e;
	}
	if (strcmp(name, "-false") == 0) {
		e->pred = PRED_FALSE;
		e->eval = pred_false;
		e->cost = COST_FAST;
		e->prob = 0.0f;
		return e;
	}
	if (strcmp(name, "-maxdepth") == 0 || strcmp(name, "-mindepth") == 0) {
		int is_max = strcmp(name, "-maxdepth") == 0;
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		int v = parse_nonneg(arg);
		if (v < 0) {
			set_errorf(ps, "Expected a positive decimal integer argument "
				       "to %s, but got %s%s%s",
				   name, q_open(), arg, q_close());
			return NULL;
		}
		advance(ps);
		if (is_max)
			ps->opts->maxdepth = v;
		else
			ps->opts->mindepth = v;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-depth") == 0 || strcmp(name, "-d") == 0) {
		ps->opts->depth_first = 1;
		ps->opts->explicit_depth = 1;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-xdev") == 0 || strcmp(name, "-mount") == 0) {
		ps->opts->xdev = 1;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-noleaf") == 0) {
		ps->opts->noleaf = 1;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-daystart") == 0) {
		apply_daystart(ps);
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-follow") == 0) {
		ps->opts->follow = 1;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-ignore_readdir_race") == 0) {
		ps->opts->ignore_readdir_race = 1;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-noignore_readdir_race") == 0) {
		ps->opts->ignore_readdir_race = 0;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-warn") == 0) {
		ps->opts->warn = 1;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-nowarn") == 0) {
		ps->opts->warn = 0;
		return mk_option_leaf(ps);
	}
	if (strcmp(name, "-prune") == 0) {
		e->pred = PRED_PRUNE;
		e->eval = pred_prune;
		e->pure = false; /* control-flow effect; pin against reordering */
		e->cost = COST_FAST;
		e->prob = 1.0f;
		return e;
	}
	if (strcmp(name, "-print") == 0 || strcmp(name, "-print0") == 0) {
		bool zero = strcmp(name, "-print0") == 0;
		e->pred = zero ? ACT_PRINT0 : ACT_PRINT;
		e->eval = act_print;
		e->pure = false;
		e->u.pf.dest = NULL;
		e->u.pf.zero = zero;
		e->cost = COST_PRINT;
		e->prob = 1.0f;
		ps->has_action = true;
		return e;
	}
	if (strcmp(name, "-printf") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		const char *ferr = NULL;
		struct fmt *cf = fmt_compile(arg, ps->arena, &ferr);
		if (!cf) {
			ps->error = ferr ? ferr : "invalid -printf format";
			return NULL;
		}
		e->pred = ACT_PRINTF;
		e->eval = act_printf;
		e->pure = false;
		e->u.pf.fmt = cf;
		e->needs_stat = fmt_needs_stat(cf);
		e->cost = COST_PRINT;
		e->prob = 1.0f;
		ps->has_action = true;
		return e;
	}
	if (strcmp(name, "-ls") == 0) {
		e->pred = ACT_LS;
		e->eval = act_ls;
		e->pure = false;
		e->u.pf.dest = NULL;
		e->needs_stat = true;
		e->cost = COST_PRINT;
		e->prob = 1.0f;
		ps->has_action = true;
		return e;
	}
	if (strcmp(name, "-fprint") == 0 || strcmp(name, "-fprint0") == 0 ||
	    strcmp(name, "-fls") == 0 || strcmp(name, "-fprintf") == 0) {
		const char *file = cur(ps);
		if (!file) {
			set_errorf(ps, "missing argument to `%s'", name);
			return NULL;
		}
		advance(ps);
		struct fmt *cf = NULL;
		if (strcmp(name, "-fprintf") == 0) {
			const char *farg = cur(ps);
			if (!farg) {
				/* file consumed, format missing: find blames the file */
				set_errorf(ps, "invalid argument `%s' to `%s'", file, name);
				return NULL;
			}
			advance(ps);
			const char *ferr = NULL;
			cf = fmt_compile(farg, ps->arena, &ferr);
			if (!cf) {
				ps->error = ferr ? ferr : "invalid -fprintf format";
				return NULL;
			}
		}
		struct outfile *dest = frt_outfile_open(file, ps->outfiles);
		if (!dest) {
			int err = errno;
			set_errorf(ps, "%s%s%s: %s", q_open(), file, q_close(), strerror(err));
			return NULL;
		}
		e->pure = false;
		e->u.pf.dest = dest;
		e->u.pf.fmt = cf;
		e->cost = COST_PRINT;
		e->prob = 1.0f;
		ps->has_action = true;
		if (strcmp(name, "-fls") == 0) {
			e->pred = ACT_LS;
			e->eval = act_ls;
			e->needs_stat = true;
		} else if (strcmp(name, "-fprintf") == 0) {
			e->pred = ACT_PRINTF;
			e->eval = act_printf;
			e->needs_stat = fmt_needs_stat(cf);
		} else {
			e->pred = ACT_PRINT;
			e->eval = act_print;
			e->u.pf.zero = (strcmp(name, "-fprint0") == 0);
		}
		return e;
	}
	if (strcmp(name, "-exec") == 0)
		return parse_exec(ps, e, 0, 0);
	if (strcmp(name, "-execdir") == 0)
		return parse_exec(ps, e, 1, 0);
	if (strcmp(name, "-ok") == 0)
		return parse_exec(ps, e, 0, 1);
	if (strcmp(name, "-okdir") == 0)
		return parse_exec(ps, e, 1, 1);
	if (strcmp(name, "-delete") == 0) {
		ps->opts->depth_first = 1; /* -delete implies -depth */
		e->pred = ACT_DELETE;
		e->eval = act_delete;
		e->pure = false;
		e->cost = COST_STAT;
		e->prob = 1.0f;
		ps->has_action = true;
		return e;
	}
	if (strcmp(name, "-quit") == 0) {
		e->pred = ACT_QUIT;
		e->eval = act_quit;
		e->pure = false; /* control-flow side effect: pin */
		/* -quit does NOT suppress the implicit -print (audit 01). */
		e->cost = COST_FAST;
		e->prob = 1.0f;
		return e;
	}

	/* A non-dash token where a predicate was expected is a misplaced path
	 * (find: "paths must precede expression"); reserve "unknown predicate" for
	 * '-'-prefixed tokens. ( ) ! , are consumed by the parser before here. */
	if (name[0] != '-') {
		set_errorf(ps, "paths must precede expression: `%s'", name);
		return NULL;
	}
	set_errorf(ps, "unknown predicate `%s'", name);
	return NULL;
}

/* ---- recursive descent (precedence: comma < or < and < not < primary) ------
 *
 * `prev` threads the token immediately before the operand a level is parsing
 * (the operator just consumed, a leading `!`, or `(`), so a missing operand can
 * be reported find's way: a binary operator found where a primary is expected
 * is "...with nothing before it"; running off the end names the previous token. */

static struct expr *parse_comma(struct pstate *ps, const char *prev);

static bool is_binop(const char *t)
{
	return t && (tok_is(t, "-o", "-or") || tok_is(t, "-a", "-and") ||
		     strcmp(t, ",") == 0);
}

static bool starts_primary(const char *t)
{
	if (!t)
		return false;
	if (strcmp(t, ")") == 0)
		return false;
	if (is_binop(t))
		return false;
	return true; /* "(", "!", "-not", or a predicate token */
}

static struct expr *parse_primary(struct pstate *ps, const char *prev)
{
	const char *t = cur(ps);
	if (strcmp(t ? t : "", "(") == 0) {
		advance(ps);
		if (++ps->paren_depth > FRT_EXPR_DEPTH_MAX) {
			set_errorf(ps, "expression nesting too deep (limit %d)",
				   FRT_EXPR_DEPTH_MAX);
			return NULL;
		}
		if (!cur(ps)) {
			set_errorf(ps, "invalid expression; expected to find a ')' but "
				       "didn't see one. Perhaps you need an extra "
				       "predicate after '('");
			return NULL;
		}
		if (strcmp(cur(ps), ")") == 0) {
			set_errorf(ps, "invalid expression; empty parentheses are not "
				       "allowed.");
			return NULL;
		}
		struct expr *e = parse_comma(ps, "(");
		if (!e)
			return NULL;
		ps->paren_depth--;
		if (!cur(ps) || strcmp(cur(ps), ")") != 0) {
			ps->error = "invalid expression; I was expecting to find a ')' somewhere "
				    "but did not see one.";
			return NULL;
		}
		advance(ps);
		return e;
	}
	(void)prev;
	return parse_predicate(ps);
}

static struct expr *parse_not(struct pstate *ps, const char *prev)
{
	int neg = 0;
	while (cur(ps) && (strcmp(cur(ps), "!") == 0 || strcmp(cur(ps), "-not") == 0)) {
		prev = cur(ps);
		neg++;
		advance(ps);
	}
	const char *t = cur(ps);
	if (!starts_primary(t)) {
		if (is_binop(t))
			set_errorf(ps, "invalid expression; you have used a binary "
				       "operator '%s' with nothing before it.", t);
		else if (t && strcmp(t, ")") == 0)
			set_errorf(ps, "expected an expression between '%s' and ')'",
				   prev ? prev : "(");
		else
			set_errorf(ps, "expected an expression after '%s'",
				   prev ? prev : "(");
		return NULL;
	}
	struct expr *e = parse_primary(ps, prev);
	if (!e)
		return NULL;
	for (int k = 0; k < neg; k++)
		e = mk_unop(ps, EXPR_NOT, e);
	return e;
}

static struct expr *parse_and(struct pstate *ps, const char *prev)
{
	struct expr *left = parse_not(ps, prev);
	if (!left)
		return NULL;
	int n = 0;
	for (;;) {
		const char *t = cur(ps);
		struct expr *right;
		if (tok_is(t, "-a", "-and")) {
			const char *op = t;
			advance(ps);
			right = parse_not(ps, op);
		} else if (starts_primary(t)) {
			right = parse_not(ps, t); /* juxtaposition = implicit AND */
		} else {
			break;
		}
		if (!right)
			return NULL;
		if (++n > FRT_EXPR_DEPTH_MAX) {
			set_errorf(ps, "expression too large (limit %d operands)",
				   FRT_EXPR_DEPTH_MAX);
			return NULL;
		}
		left = mk_binop(ps, EXPR_AND, left, right);
	}
	return left;
}

static struct expr *parse_or(struct pstate *ps, const char *prev)
{
	struct expr *left = parse_and(ps, prev);
	if (!left)
		return NULL;
	int n = 0;
	while (tok_is(cur(ps), "-o", "-or")) {
		const char *op = cur(ps);
		advance(ps);
		struct expr *right = parse_and(ps, op);
		if (!right)
			return NULL;
		if (++n > FRT_EXPR_DEPTH_MAX) {
			set_errorf(ps, "expression too large (limit %d operands)",
				   FRT_EXPR_DEPTH_MAX);
			return NULL;
		}
		left = mk_binop(ps, EXPR_OR, left, right);
	}
	return left;
}

static struct expr *parse_comma(struct pstate *ps, const char *prev)
{
	struct expr *left = parse_or(ps, prev);
	if (!left)
		return NULL;
	int n = 0;
	while (cur(ps) && strcmp(cur(ps), ",") == 0) {
		const char *op = cur(ps);
		advance(ps);
		struct expr *right = parse_or(ps, op);
		if (!right)
			return NULL;
		if (++n > FRT_EXPR_DEPTH_MAX) {
			set_errorf(ps, "expression too large (limit %d operands)",
				   FRT_EXPR_DEPTH_MAX);
			return NULL;
		}
		left = mk_binop(ps, EXPR_COMMA, left, right);
	}
	return left;
}

/* ---- implicit -print ------------------------------------------------------- */

static struct expr *mk_print_leaf(struct pstate *ps)
{
	struct expr *e = new_node(ps, EXPR_LEAF);
	e->pred = ACT_PRINT;
	e->eval = act_print;
	e->pure = false;
	e->cost = COST_PRINT;
	e->prob = 1.0f;
	return e;
}

/* ---- top level ------------------------------------------------------------- */

static bool looks_like_expr(const char *t)
{
	/* ')' and ',' cannot begin an expression in find's grammar, so a leading
	 * one is a path (find walks it, then errors on the bad name). Only '-foo',
	 * '(' and '!' start the expression. */
	return t[0] == '-' || strcmp(t, "(") == 0 || strcmp(t, "!") == 0;
}

/* -D help: list the debug flags (find's util.c, %-10s %s, same order). */
static void print_debug_help(void)
{
	static const struct {
		const char *name, *doc;
	} d[] = {
		{"exec", "Show diagnostic information relating to -exec, -execdir, -ok and -okdir"},
		{"opt", "Show diagnostic information relating to optimisation"},
		{"rates", "Indicate how often each predicate succeeded"},
		{"search", "Navigate the directory tree verbosely"},
		{"stat", "Trace calls to stat(2) and lstat(2)"},
		{"time", "Show diagnostic information relating to time-of-day and timestamp comparisons"},
		{"tree", "Display the expression tree"},
		{"all", "Set all of the debug flags (but help)"},
		{"help", "Explain the various -D options"},
	};
	fputs("Valid arguments for -D:\n", stdout);
	for (size_t k = 0; k < sizeof d / sizeof *d; k++)
		printf("%-10s %s\n", d[k].name, d[k].doc);
}

/* find's check_option_combinations: walk the expression for -delete and -prune. */
static void scan_delete_prune(const struct expr *e, int *del, int *prune)
{
	if (!e)
		return;
	if (e->kind == EXPR_LEAF) {
		if (e->pred == ACT_DELETE)
			*del = 1;
		else if (e->pred == PRED_PRUNE)
			*prune = 1;
		return;
	}
	scan_delete_prune(e->lhs, del, prune);
	scan_delete_prune(e->rhs, del, prune);
}

int frt_parse(int argc, char **argv, struct arena *arena, struct parse_result *out)
{
	memset(out, 0, sizeof *out);
	out->opts.follow = 0;
	out->opts.optlevel = 3;
	out->opts.maxdepth = -1;
	out->opts.mindepth = 0;
	out->opts.threads = -1; /* auto: engage the stat pool only when structural (main.c) */

	int i = 1;

	/* leading global options (must precede paths; full set in sprint 03) */
	while (i < argc) {
		const char *a = argv[i];
		if (strcmp(a, "-P") == 0) {
			out->opts.follow = 0;
			i++;
		} else if (strcmp(a, "-L") == 0 || strcmp(a, "-follow") == 0) {
			out->opts.follow = 1;
			i++;
		} else if (strcmp(a, "-H") == 0) {
			out->opts.follow = 2;
			i++;
		} else if (a[0] == '-' && a[1] == 'O') {
			/* find accepts any -O<decimal integer> (clamped internally) with
			 * three distinct errors for the malformed forms. */
			const char *tail = a + 2;
			if (*tail == '\0') {
				out->error = "The -O option must be immediately followed by a "
					     "decimal integer";
				return -1;
			}
			if (*tail < '0' || *tail > '9') {
				out->error = "Please specify a decimal number immediately after -O";
				return -1;
			}
			char *end;
			errno = 0;
			unsigned long lvl = strtoul(tail, &end, 10);
			if (*end != '\0') {
				char *m = arena_alloc(arena, strlen(tail) + 40);
				snprintf(m, strlen(tail) + 40, "Invalid optimisation level %s", tail);
				out->error = m;
				return -1;
			}
			if (lvl == ULONG_MAX && errno == ERANGE) {
				/* find passes errno to error(), appending ": <strerror>". */
				char *m = arena_alloc(arena, strlen(tail) + 64);
				snprintf(m, strlen(tail) + 64, "Invalid optimisation level %s: %s",
					 tail, strerror(ERANGE));
				out->error = m;
				return -1;
			}
			if (lvl > USHRT_MAX) {
				char *m = arena_alloc(arena, 128);
				snprintf(m, 128, "Optimisation level %lu is too high.  If you "
					 "want to find files very quickly, consider using GNU "
					 "locate.", lvl);
				out->error = m;
				return -1;
			}
			if (lvl > 4) /* clamp to the implemented max; output is -O-invariant */
				lvl = 4;
			out->opts.optlevel = (int)lvl;
			i++;
		} else if (strcmp(a, "--ferret-threads") == 0 && i + 1 < argc) {
			/* ferret extension (kept out of --help): worker count for the
			 * parallel stat pass. 0 = force all CPUs, 1 = force serial, N = N.
			 * Omitting the flag leaves threads at -1 (structural auto, main.c). */
			int v = parse_nonneg(argv[i + 1]);
			out->opts.threads = (v < 0) ? 1 : v;
			i += 2;
		} else if (strcmp(a, "-D") == 0 && i + 1 < argc) {
			const char *d = argv[i + 1];
			/* comma-separated debug words; honor tree/opt/all, print help, and
			 * accept search/stat/rates/exec/time as no-ops. find warns (to
			 * stderr, rc unchanged) on any unrecognised word. */
			int want_help = 0;
			for (const char *p = d; *p;) {
				const char *q = p;
				while (*q && *q != ',')
					q++;
				size_t len = (size_t)(q - p);
#define WORD_IS(s) (len == strlen(s) && strncmp(p, s, len) == 0)
				if (WORD_IS("tree"))
					out->opts.debug |= FRT_DBG_TREE;
				else if (WORD_IS("opt"))
					out->opts.debug |= FRT_DBG_OPT;
				else if (WORD_IS("all"))
					out->opts.debug |= FRT_DBG_TREE | FRT_DBG_OPT;
				else if (WORD_IS("help"))
					want_help = 1;
				else if (WORD_IS("search") || WORD_IS("stat") ||
					 WORD_IS("rates") || WORD_IS("exec") || WORD_IS("time"))
					; /* recognised, no-op */
				else
					fprintf(stderr,
						"ferret: Ignoring unrecognised debug flag %s%.*s%s\n",
						q_open(), (int)len, p, q_close());
#undef WORD_IS
				p = *q ? q + 1 : q;
			}
			if (want_help) { /* find: print the list and exit, no traversal */
				print_debug_help();
				exit(EXIT_SUCCESS);
			}
			i += 2;
		} else {
			break;
		}
	}

	/* start paths: up to the first token that looks like the expression */
	int path_start = i;
	while (i < argc && !looks_like_expr(argv[i]))
		i++;
	int npaths = i - path_start;

	if (npaths == 0) {
		out->paths = arena_alloc(arena, sizeof(char *));
		out->paths[0] = (char *)".";
		out->npaths = 1;
	} else {
		out->paths = arena_alloc(arena, (size_t)npaths * sizeof(char *));
		for (int k = 0; k < npaths; k++)
			out->paths[k] = argv[path_start + k];
		out->npaths = npaths;
	}

	/* expression */
	struct pstate ps = {
		.argv = argv,
		.argc = argc,
		.i = i,
		.arena = arena,
		.opts = &out->opts,
		.outfiles = &out->outfiles,
		.error = NULL,
		.has_action = false,
		.full_days = 0,
	};
	/* find captures start_time once; cur_day_start defaults to a day earlier. */
	clock_gettime(CLOCK_REALTIME, &ps.start_time);
	ps.cur_day_start.tv_sec = ps.start_time.tv_sec - DAYSECS;
	ps.cur_day_start.tv_nsec = ps.start_time.tv_nsec;

	struct expr *expr = NULL;
	if (ps.i < ps.argc) {
		expr = parse_comma(&ps, NULL);
		if (!expr) {
			out->error = ps.error ? ps.error : "invalid expression";
			return -1;
		}
		if (ps.i < ps.argc) {
			/* leftover tokens: a dangling ')' here means an unmatched close
			 * (the unmatched-open case is handled inside parse_primary). */
			out->error = ps.error ? ps.error : "invalid expression";
			if (strcmp(argv[ps.i], ")") == 0)
				out->error = "you have too many ')'";
			return -1;
		}
	}

	/* implicit -print: appended only when no action appears (audit 01). */
	if (!expr) {
		out->expr = mk_print_leaf(&ps);
	} else if (!ps.has_action) {
		out->expr = mk_binop(&ps, EXPR_AND, expr, mk_print_leaf(&ps));
	} else {
		out->expr = expr;
	}

	/* -delete implicitly turns on -depth, which makes -prune a no-op. find
	 * treats the combination as a fatal error unless -depth was explicit, to
	 * guard against deleting more than the user expected (Savannah #20865). */
	int has_delete = 0, has_prune = 0;
	scan_delete_prune(out->expr, &has_delete, &has_prune);
	if (has_delete && has_prune && !out->opts.explicit_depth) {
		out->error = "The -delete action automatically turns on -depth, "
			     "but -prune does nothing when -depth is in effect.  "
			     "If you want to carry on anyway, just explicitly use "
			     "the -depth option.";
		return -1;
	}
	return 0;
}
