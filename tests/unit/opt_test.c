#include "test.h"
#include "parse.h"
#include "opt.h"
#include "expr.h"
#include "arena.h"

static struct expr *opt(struct arena *a, int level, int argc, char **argv)
{
	struct parse_result pr;
	frt_parse(argc, argv, a, &pr);
	return frt_optimize(pr.expr, level, a);
}

int main(void)
{
	struct arena a;
	arena_init(&a, 0);

	/* reorder: cheap -name before expensive -size in a conjunction.
	 * expr = AND(AND(size,name), print) -> AND(AND(name,size), print) at O2. */
	{
		char *argv[] = {"ferret", ".", "-size", "+1k", "-name", "x"};
		struct expr *e = opt(&a, 2, 6, argv);
		CHECK("top AND", e->kind == EXPR_AND && e->rhs->pred == ACT_PRINT);
		struct expr *user = e->lhs; /* AND(name, size) after reorder */
		CHECK("user AND", user->kind == EXPR_AND);
		CHECK("name moved first", user->lhs->kind == EXPR_LEAF &&
					  user->lhs->pred == PRED_NAME);
		CHECK("size second", user->rhs->pred == PRED_SIZE);
	}

	/* O0: no reorder — size stays first. */
	{
		char *argv[] = {"ferret", ".", "-size", "+1k", "-name", "x"};
		struct expr *e = opt(&a, 0, 6, argv);
		struct expr *user = e->lhs;
		CHECK("O0 size first", user->lhs->pred == PRED_SIZE);
	}

	/* actions are pinned: -print before -name must NOT reorder. */
	{
		char *argv[] = {"ferret", ".", "-print", "-name", "x"};
		struct expr *e = opt(&a, 3, 5, argv);
		/* expr is AND(print, name) (no implicit print; print is the action).
		 * print is impure -> stays first even though name is cheaper. */
		CHECK("print pinned first", e->kind == EXPR_AND &&
					    e->lhs->pred == ACT_PRINT);
	}

	/* O3 fold: -true dropped from a conjunction. */
	{
		char *argv[] = {"ferret", ".", "-true", "-name", "x", "-print"};
		struct expr *e = opt(&a, 3, 6, argv);
		/* the -true no-op should be gone; remaining: AND(name, print) */
		CHECK("true folded", e->kind == EXPR_AND && e->lhs->pred == PRED_NAME &&
				     e->rhs->pred == ACT_PRINT);
	}

	arena_destroy(&a);
	return test_summary("opt");
}
