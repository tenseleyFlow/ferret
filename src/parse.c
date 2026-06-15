#include "parse.h"
#include "pred.h"
#include "action.h"
#include "glob.h"

#include <stdlib.h>
#include <string.h>

/* bfs cost constants (audit 03, decided). */
#define COST_FAST 40.0f
#define COST_FNMATCH 400.0f
#define COST_STAT 1000.0f
#define COST_PRINT 20000.0f

/* ---- parser state ---------------------------------------------------------- */

struct pstate {
	char **argv;
	int argc;
	int i;
	struct arena *arena;
	struct options *opts;  /* positional options write here */
	const char *error;     /* set on failure */
	const char *error_arg; /* offending token, if any */
	bool has_action;       /* any action seen => suppress implicit -print */
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

static int parse_type_list(const char *arg, unsigned *mask_out)
{
	unsigned mask = 0;
	for (const char *p = arg; *p; p++) {
		switch (*p) {
		case 'b': mask |= 1u << FRT_BLK; break;
		case 'c': mask |= 1u << FRT_CHR; break;
		case 'd': mask |= 1u << FRT_DIR; break;
		case 'p': mask |= 1u << FRT_FIFO; break;
		case 'f': mask |= 1u << FRT_REG; break;
		case 'l': mask |= 1u << FRT_LNK; break;
		case 's': mask |= 1u << FRT_SOCK; break;
		case ',': continue; /* list separator */
		default: return -1;
		}
		/* after a type letter, the only valid follower is ',' or end */
		if (p[1] && p[1] != ',')
			return -1;
	}
	if (mask == 0)
		return -1;
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
			ps->error = "missing argument to -name";
			return NULL;
		}
		advance(ps);
		e->pred = name[1] == 'i' ? PRED_INAME : PRED_NAME;
		e->eval = pred_name;
		e->u.name.pattern = arena_strdup(ps->arena, arg);
		e->u.name.glob_flags = (name[1] == 'i') ? FRT_GLOB_CASEFOLD : 0;
		e->cost = COST_FNMATCH;
		e->prob = 0.5f;
		return e;
	}
	if (strcmp(name, "-type") == 0) {
		const char *arg = cur(ps);
		if (!arg) {
			ps->error = "missing argument to -type";
			return NULL;
		}
		advance(ps);
		unsigned mask;
		if (parse_type_list(arg, &mask) != 0) {
			ps->error = "unknown argument to -type";
			ps->error_arg = arg;
			return NULL;
		}
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
		int v = parse_nonneg(arg);
		if (v < 0) {
			ps->error = is_max
				? "Expected a positive decimal integer argument to -maxdepth"
				: "Expected a positive decimal integer argument to -mindepth";
			ps->error_arg = arg;
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
		e->eval = zero ? act_print0 : act_print;
		e->pure = false;
		e->no_default_print = true;
		e->cost = COST_PRINT;
		e->prob = 1.0f;
		ps->has_action = true;
		return e;
	}

	ps->error = "unknown predicate";
	ps->error_arg = name;
	return NULL;
}

/* ---- recursive descent (precedence: comma < or < and < not < primary) ------ */

static struct expr *parse_comma(struct pstate *ps);

static bool starts_primary(const char *t)
{
	if (!t)
		return false;
	if (strcmp(t, ")") == 0)
		return false;
	if (tok_is(t, "-o", "-or") || tok_is(t, "-a", "-and") || strcmp(t, ",") == 0)
		return false;
	return true; /* "(", "!", "-not", or a predicate token */
}

static struct expr *parse_primary(struct pstate *ps)
{
	const char *t = cur(ps);
	if (strcmp(t ? t : "", "(") == 0) {
		advance(ps);
		struct expr *e = parse_comma(ps);
		if (!e)
			return NULL;
		if (!cur(ps) || strcmp(cur(ps), ")") != 0) {
			ps->error = "invalid expression; I was expecting to find a ')' somewhere "
				    "but did not see one.";
			return NULL;
		}
		advance(ps);
		return e;
	}
	return parse_predicate(ps);
}

static struct expr *parse_not(struct pstate *ps)
{
	int neg = 0;
	while (cur(ps) && (strcmp(cur(ps), "!") == 0 || strcmp(cur(ps), "-not") == 0)) {
		neg++;
		advance(ps);
	}
	if (!starts_primary(cur(ps))) {
		ps->error = "expected an expression after '!'";
		return NULL;
	}
	struct expr *e = parse_primary(ps);
	if (!e)
		return NULL;
	for (int k = 0; k < neg; k++)
		e = mk_unop(ps, EXPR_NOT, e);
	return e;
}

static struct expr *parse_and(struct pstate *ps)
{
	struct expr *left = parse_not(ps);
	if (!left)
		return NULL;
	for (;;) {
		const char *t = cur(ps);
		if (tok_is(t, "-a", "-and")) {
			advance(ps);
			struct expr *right = parse_not(ps);
			if (!right)
				return NULL;
			left = mk_binop(ps, EXPR_AND, left, right);
		} else if (starts_primary(t)) {
			/* juxtaposition = implicit AND */
			struct expr *right = parse_not(ps);
			if (!right)
				return NULL;
			left = mk_binop(ps, EXPR_AND, left, right);
		} else {
			break;
		}
	}
	return left;
}

static struct expr *parse_or(struct pstate *ps)
{
	struct expr *left = parse_and(ps);
	if (!left)
		return NULL;
	while (tok_is(cur(ps), "-o", "-or")) {
		advance(ps);
		struct expr *right = parse_and(ps);
		if (!right)
			return NULL;
		left = mk_binop(ps, EXPR_OR, left, right);
	}
	return left;
}

static struct expr *parse_comma(struct pstate *ps)
{
	struct expr *left = parse_or(ps);
	if (!left)
		return NULL;
	while (cur(ps) && strcmp(cur(ps), ",") == 0) {
		advance(ps);
		struct expr *right = parse_or(ps);
		if (!right)
			return NULL;
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
	e->no_default_print = true;
	e->cost = COST_PRINT;
	e->prob = 1.0f;
	return e;
}

/* ---- top level ------------------------------------------------------------- */

static bool looks_like_expr(const char *t)
{
	return t[0] == '-' || strcmp(t, "(") == 0 || strcmp(t, ")") == 0 ||
	       strcmp(t, "!") == 0 || strcmp(t, ",") == 0;
}

int frt_parse(int argc, char **argv, struct arena *arena, struct parse_result *out)
{
	memset(out, 0, sizeof *out);
	out->opts.follow = 0;
	out->opts.optlevel = 3;
	out->opts.maxdepth = -1;
	out->opts.mindepth = 0;

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
		} else if (a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '9' &&
			   a[3] == '\0') {
			out->opts.optlevel = a[2] - '0';
			i++;
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
		.error = NULL,
		.error_arg = NULL,
		.has_action = false,
	};

	struct expr *expr = NULL;
	if (ps.i < ps.argc) {
		expr = parse_comma(&ps);
		if (!expr) {
			out->error = ps.error ? ps.error : "invalid expression";
			out->error_arg = ps.error_arg;
			return -1;
		}
		if (ps.i < ps.argc) {
			/* leftover tokens (e.g. a stray ')') */
			out->error = ps.error ? ps.error : "invalid expression";
			out->error_arg = cur(&ps);
			if (!out->error_arg)
				out->error_arg = NULL;
			if (strcmp(argv[ps.i], ")") == 0)
				out->error = "invalid expression; I was expecting to find a "
					     "')' somewhere but did not see one.";
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
	return 0;
}
