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

	/* positional options write into opts and parse as a true no-op leaf */
	{
		char *argv[] = {"ferret", ".", "-maxdepth", "3", "-mindepth", "1",
				"-depth", "-xdev", "-name", "x"};
		struct parse_result pr = parse(&a, 10, argv);
		CHECK("maxdepth set", pr.opts.maxdepth == 3);
		CHECK("mindepth set", pr.opts.mindepth == 1);
		CHECK("depth_first set", pr.opts.depth_first == 1);
		CHECK("xdev set", pr.opts.xdev == 1);
	}
	{
		char *argv[] = {"ferret", "-L", ".", "-type", "f"};
		struct parse_result pr = parse(&a, 5, argv);
		CHECK("follow -L", pr.opts.follow == 1);
	}
	{
		char *argv[] = {"ferret", ".", "-maxdepth", "notanumber"};
		struct parse_result pr;
		int rc = frt_parse(4, argv, &a, &pr);
		CHECK("bad maxdepth fails", rc == -1 && pr.error != NULL);
	}

	/* -size payload: +1k -> GT, val 1, unit 1024 */
	{
		char *argv[] = {"ferret", ".", "-size", "+1k"};
		struct parse_result pr = parse(&a, 4, argv);
		struct expr *e = pr.expr->lhs; /* under implicit-print AND */
		CHECK("size leaf", e->kind == EXPR_LEAF && e->pred == PRED_SIZE);
		CHECK("size GT", e->u.size.kind == COMP_GT);
		CHECK("size val", e->u.size.val == 1);
		CHECK("size unit 1024", e->u.size.unit == 1024);
		CHECK("size needs_stat", e->needs_stat);
	}
	/* -perm octal exact */
	{
		char *argv[] = {"ferret", ".", "-perm", "644"};
		struct parse_result pr = parse(&a, 4, argv);
		struct expr *e = pr.expr->lhs;
		CHECK("perm exact", e->pred == PRED_PERM && e->u.perm.match == PERM_EXACT);
		CHECK("perm mode 0644", e->u.perm.mode == 0644);
	}
	/* -perm -u+x (all-bits, symbolic) */
	{
		char *argv[] = {"ferret", ".", "-perm", "-u+x"};
		struct parse_result pr = parse(&a, 4, argv);
		struct expr *e = pr.expr->lhs;
		CHECK("perm all", e->u.perm.match == PERM_ALL);
		CHECK("perm u+x = 0100", e->u.perm.mode == 0100);
	}
	/* -perm /222 (any-bits) */
	{
		char *argv[] = {"ferret", ".", "-perm", "/222"};
		struct parse_result pr = parse(&a, 4, argv);
		struct expr *e = pr.expr->lhs;
		CHECK("perm any", e->u.perm.match == PERM_ANY);
		CHECK("perm /222 = 0222", e->u.perm.mode == 0222);
	}
	/* -links -2 -> LT */
	{
		char *argv[] = {"ferret", ".", "-links", "-2"};
		struct parse_result pr = parse(&a, 4, argv);
		struct expr *e = pr.expr->lhs;
		CHECK("links LT", e->pred == PRED_LINKS && e->u.num.kind == COMP_LT &&
				  e->u.num.val == 2);
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
