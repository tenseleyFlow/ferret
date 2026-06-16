#include "idcache.h"
#include "util.h"

#include <grp.h>
#include <pwd.h>
#include <stddef.h>

/* name == NULL is a cached negative result (no such id). Distinct ids in a
 * traversal are few, so a linear scan beats the syscall it replaces. */
struct ident {
	unsigned long id;
	char *name;
};

static struct ident *u_tab;
static size_t u_n, u_cap;
static struct ident *g_tab;
static size_t g_n, g_cap;

static const char *cache_insert(struct ident **tab, size_t *n, size_t *cap,
				unsigned long id, const char *resolved)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		*tab = frt_xrealloc(*tab, *cap * sizeof **tab);
	}
	(*tab)[*n].id = id;
	(*tab)[*n].name = resolved ? frt_strdup(resolved) : NULL;
	return (*tab)[(*n)++].name;
}

const char *frt_uid_name(uid_t uid)
{
	for (size_t i = 0; i < u_n; i++)
		if (u_tab[i].id == uid)
			return u_tab[i].name;
	struct passwd *pw = getpwuid(uid);
	return cache_insert(&u_tab, &u_n, &u_cap, uid, pw ? pw->pw_name : NULL);
}

const char *frt_gid_name(gid_t gid)
{
	for (size_t i = 0; i < g_n; i++)
		if (g_tab[i].id == gid)
			return g_tab[i].name;
	struct group *gr = getgrgid(gid);
	return cache_insert(&g_tab, &g_n, &g_cap, gid, gr ? gr->gr_name : NULL);
}
