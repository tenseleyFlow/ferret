#include "opt.h"
#include "util.h"

#include <stdio.h>

/* ---- cost/probability annotation (bottom-up) ------------------------------- */

static void annotate(struct expr *e)
{
	switch (e->kind) {
	case EXPR_LEAF:
		return; /* cost/prob/pure set at parse time */
	case EXPR_NOT:
		annotate(e->lhs);
		e->cost = e->lhs->cost;
		e->prob = 1.0f - e->lhs->prob;
		e->pure = e->lhs->pure;
		return;
	case EXPR_AND:
		annotate(e->lhs);
		annotate(e->rhs);
		e->cost = e->lhs->cost + e->lhs->prob * e->rhs->cost;
		e->prob = e->lhs->prob * e->rhs->prob;
		e->pure = e->lhs->pure && e->rhs->pure;
		return;
	case EXPR_OR:
		annotate(e->lhs);
		annotate(e->rhs);
		e->cost = e->lhs->cost + (1.0f - e->lhs->prob) * e->rhs->cost;
		e->prob = e->lhs->prob + e->rhs->prob - e->lhs->prob * e->rhs->prob;
		e->pure = e->lhs->pure && e->rhs->pure;
		return;
	case EXPR_COMMA:
		annotate(e->lhs);
		annotate(e->rhs);
		e->cost = e->lhs->cost + e->rhs->cost;
		e->prob = e->rhs->prob;
		e->pure = e->lhs->pure && e->rhs->pure;
		return;
	}
}

/* ---- flatten an AND/OR chain into its operand list ------------------------- */

static void flatten(struct expr *e, enum expr_kind kind, struct expr **ops, int *n)
{
	if (e->kind == kind) {
		flatten(e->lhs, kind, ops, n);
		flatten(e->rhs, kind, ops, n);
	} else {
		ops[(*n)++] = e;
	}
}

/* Number of operands in a maximal `kind` chain (== the count flatten() emits). */
static int chain_len(const struct expr *e, enum expr_kind kind)
{
	if (e->kind == kind)
		return chain_len(e->lhs, kind) + chain_len(e->rhs, kind);
	return 1;
}

/* A before B is cheaper? For AND we want cheap, likely-to-fail tests first; for
 * OR, cheap likely-to-succeed first. Pairwise comparison of the two orderings. */
static int prefer_ab(enum expr_kind kind, const struct expr *a, const struct expr *b)
{
	if (kind == EXPR_AND) {
		float ab = a->cost + a->prob * b->cost;
		float ba = b->cost + b->prob * a->cost;
		return ab <= ba;
	}
	float ab = a->cost + (1.0f - a->prob) * b->cost;
	float ba = b->cost + (1.0f - b->prob) * a->cost;
	return ab <= ba;
}

/* Stable insertion sort of ops[lo,hi) by the pairwise comparator. */
static void sort_run(enum expr_kind kind, struct expr **ops, int lo, int hi)
{
	for (int i = lo + 1; i < hi; i++) {
		struct expr *cur = ops[i];
		int j = i - 1;
		while (j >= lo && !prefer_ab(kind, ops[j], cur)) {
			ops[j + 1] = ops[j];
			j--;
		}
		ops[j + 1] = cur;
	}
}

/* Reorder maximal runs of pure operands (impure operands are barriers). */
static void reorder(enum expr_kind kind, struct expr **ops, int n)
{
	int i = 0;
	while (i < n) {
		if (!ops[i]->pure) {
			i++;
			continue;
		}
		int j = i;
		while (j < n && ops[j]->pure)
			j++;
		sort_run(kind, ops, i, j);
		i = j;
	}
}

static int is_const(const struct expr *e, enum pred_id id)
{
	return e->kind == EXPR_LEAF && e->pred == id;
}

/* ---- driver ---------------------------------------------------------------- */

static struct expr *mk(struct arena *a, enum expr_kind kind, struct expr *l, struct expr *r)
{
	struct expr *e = arena_alloc(a, sizeof *e);
	*e = (struct expr){0};
	e->kind = kind;
	e->lhs = l;
	e->rhs = r;
	return e;
}

struct expr *frt_optimize(struct expr *e, int level, struct arena *a)
{
	if (!e)
		return e;

	if (e->kind == EXPR_NOT) {
		e->lhs = frt_optimize(e->lhs, level, a);
		annotate(e);
		return e;
	}
	if (e->kind == EXPR_COMMA) {
		e->lhs = frt_optimize(e->lhs, level, a);
		e->rhs = frt_optimize(e->rhs, level, a);
		annotate(e);
		return e;
	}
	if (e->kind != EXPR_AND && e->kind != EXPR_OR) {
		annotate(e);
		return e;
	}

	enum expr_kind kind = e->kind;

	/* Flatten the chain into an arena array sized to the exact operand count
	 * (no fixed cap — a 100k-operand `-o` chain must not overflow), then
	 * optimize each operand. The array is per-call, so recursion into nested
	 * chains allocates its own and can't clobber this one. */
	int count = chain_len(e, kind);
	struct expr **tmp = arena_alloc(a, (size_t)count * sizeof(struct expr *));
	int n = 0;
	flatten(e, kind, tmp, &n);
	for (int k = 0; k < count; k++)
		tmp[k] = frt_optimize(tmp[k], level, a);

	if (level >= 1)
		reorder(kind, tmp, n);

	/* constant folding (O3+): drop no-op identities. AND drops -true, OR drops
	 * -false (both pure no-ops, so removing them changes nothing observable). */
	if (level >= 3) {
		enum pred_id drop = (kind == EXPR_AND) ? PRED_TRUE : PRED_FALSE;
		int w = 0;
		for (int k = 0; k < n; k++)
			if (!is_const(tmp[k], drop))
				tmp[w++] = tmp[k];
		if (w > 0) /* keep at least one operand if all were the identity */
			n = w;
	}

	/* rebuild a left-associative chain */
	struct expr *res = tmp[0];
	for (int k = 1; k < n; k++)
		res = mk(a, kind, res, tmp[k]);
	annotate(res);
	return res;
}

/* ---- -D tree / -D opt dump ------------------------------------------------- */

static const char *pred_name_str(enum pred_id p)
{
	switch (p) {
	case PRED_NAME: return "name"; case PRED_INAME: return "iname";
	case PRED_TYPE: return "type"; case PRED_EMPTY: return "empty";
	case PRED_TRUE: return "true"; case PRED_FALSE: return "false";
	case PRED_PRUNE: return "prune"; case PRED_OPTION: return "option";
	case PRED_SIZE: return "size"; case PRED_LINKS: return "links";
	case PRED_INUM: return "inum"; case PRED_UID: return "uid"; case PRED_GID: return "gid";
	case PRED_NOUSER: return "nouser"; case PRED_NOGROUP: return "nogroup";
	case PRED_SAMEFILE: return "samefile"; case PRED_PERM: return "perm";
	case PRED_ACCESS: return "access"; case PRED_TIME: return "time";
	case PRED_PATH: return "path"; case PRED_LNAME: return "lname";
	case PRED_XTYPE: return "xtype"; case PRED_FSTYPE: return "fstype";
	case PRED_REGEX: return "regex";
	case PRED_CONTAINS: return "contains";
	case ACT_PRINT: return "print"; case ACT_PRINT0: return "print0";
	case ACT_EXEC: return "exec"; case ACT_DELETE: return "delete";
	case ACT_QUIT: return "quit"; case ACT_PRINTF: return "printf"; case ACT_LS: return "ls";
	}
	return "?";
}

static void dump(const struct expr *e, int depth)
{
	for (int i = 0; i < depth; i++)
		fputs("  ", stderr);
	switch (e->kind) {
	case EXPR_AND: fputs("AND\n", stderr); dump(e->lhs, depth + 1); dump(e->rhs, depth + 1); break;
	case EXPR_OR:  fputs("OR\n", stderr); dump(e->lhs, depth + 1); dump(e->rhs, depth + 1); break;
	case EXPR_NOT: fputs("NOT\n", stderr); dump(e->lhs, depth + 1); break;
	case EXPR_COMMA: fputs("COMMA\n", stderr); dump(e->lhs, depth + 1); dump(e->rhs, depth + 1); break;
	case EXPR_LEAF:
		fprintf(stderr, "%s [cost=%.0f prob=%.2f%s]\n", pred_name_str(e->pred),
			e->cost, e->prob, e->pure ? "" : " impure");
		break;
	}
}

void frt_expr_dump(const struct expr *e, const char *label)
{
	fprintf(stderr, "ferret: -D %s:\n", label);
	if (e)
		dump(e, 1);
}
