#include "test.h"
#include "parse.h"
#include "expr.h"
#include "arena.h"

static struct parse_result parse(struct arena *a, int argc, char **argv)
{
	struct parse_result pr;
	frt_parse(argc, argv, a, &pr);
	return pr;
}

int main(void)
{
	struct arena a;
	arena_init(&a, 0);

	/* default path "." when none given; empty expr -> implicit -print leaf */
	{
		char *argv[] = {"ferret"};
		struct parse_result pr = parse(&a, 1, argv);
		CHECK("default no error", pr.error == NULL);
		CHECK("default path count", pr.npaths == 1);
		CHECK_STR("default path", pr.paths[0], ".");
		CHECK("empty expr -> print leaf", pr.expr->kind == EXPR_LEAF &&
						  pr.expr->pred == ACT_PRINT);
	}

	/* explicit paths collected, stop at first expression token */
	{
		char *argv[] = {"ferret", "a", "b", "-name", "x"};
		struct parse_result pr = parse(&a, 5, argv);
		CHECK("paths count", pr.npaths == 2);
		CHECK_STR("path a", pr.paths[0], "a");
		CHECK_STR("path b", pr.paths[1], "b");
		/* -name x with no action -> AND(name, print) */
		CHECK("implicit print wrap", pr.expr->kind == EXPR_AND);
		CHECK("wrap lhs name", pr.expr->lhs->kind == EXPR_LEAF &&
				       pr.expr->lhs->pred == PRED_NAME);
		CHECK("wrap rhs print", pr.expr->rhs->kind == EXPR_LEAF &&
				       pr.expr->rhs->pred == ACT_PRINT);
	}

	/* explicit -print suppresses the implicit one */
	{
		char *argv[] = {"ferret", ".", "-print"};
		struct parse_result pr = parse(&a, 3, argv);
		CHECK("explicit print, no wrap", pr.expr->kind == EXPR_LEAF &&
						pr.expr->pred == ACT_PRINT);
	}

	/* precedence: a -o b -a c == OR(a, AND(b,c)) ; using -name leaves as a/b/c */
	{
		char *argv[] = {"ferret", ".", "-type", "f", "-o", "-type", "d",
				"-a", "-type", "l"};
		struct parse_result pr = parse(&a, 10, argv);
		/* implicit print wraps the whole thing: AND(OR(...), print) */
		CHECK("top AND(print)", pr.expr->kind == EXPR_AND &&
				       pr.expr->rhs->pred == ACT_PRINT);
		struct expr *e = pr.expr->lhs;
		CHECK("or at top of user expr", e->kind == EXPR_OR);
		CHECK("or lhs is type f leaf", e->lhs->kind == EXPR_LEAF);
		CHECK("or rhs is AND (b -a c)", e->rhs->kind == EXPR_AND);
	}

	/* left associativity: a -a b -o c == OR(AND(a,b), c) */
	{
		char *argv[] = {"ferret", ".", "-type", "f", "-a", "-type", "d",
				"-o", "-type", "l"};
		struct parse_result pr = parse(&a, 10, argv);
		struct expr *e = pr.expr->lhs; /* under the implicit-print AND */
		CHECK("top OR", e->kind == EXPR_OR);
		CHECK("OR lhs AND", e->lhs->kind == EXPR_AND);
		CHECK("OR rhs leaf", e->rhs->kind == EXPR_LEAF);
	}

	/* NOT */
	{
		char *argv[] = {"ferret", ".", "!", "-name", "x"};
		struct parse_result pr = parse(&a, 5, argv);
		struct expr *e = pr.expr->lhs;
		CHECK("NOT node", e->kind == EXPR_NOT);
		CHECK("NOT child name", e->lhs->kind == EXPR_LEAF && e->lhs->pred == PRED_NAME);
	}

	/* grouping overrides precedence: ( a -o b ) -a c */
	{
		char *argv[] = {"ferret", ".", "(", "-type", "f", "-o", "-type", "d",
				")", "-a", "-type", "l"};
		struct parse_result pr = parse(&a, 12, argv);
		struct expr *e = pr.expr->lhs;
		CHECK("group: top AND", e->kind == EXPR_AND);
		CHECK("group: lhs OR", e->lhs->kind == EXPR_OR);
		CHECK("group: rhs leaf", e->rhs->kind == EXPR_LEAF);
	}

	/* unknown predicate -> error */
	{
		char *argv[] = {"ferret", ".", "-bogus"};
		struct parse_result pr;
		int rc = frt_parse(3, argv, &a, &pr);
		CHECK("unknown predicate fails", rc == -1 && pr.error != NULL);
		CHECK_STR("error arg", pr.error_arg, "-bogus");
	}

	/* missing close paren -> error */
	{
		char *argv[] = {"ferret", ".", "(", "-name", "x"};
		struct parse_result pr;
		int rc = frt_parse(5, argv, &a, &pr);
		CHECK("missing paren fails", rc == -1 && pr.error != NULL);
	}

	arena_destroy(&a);
	return test_summary("parse");
}
