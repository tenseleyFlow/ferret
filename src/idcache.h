#ifndef FRT_IDCACHE_H
#define FRT_IDCACHE_H

#include <sys/types.h>

/*
 * Cached uid/gid -> name lookups. getpwuid()/getgrgid() consult (and on most
 * systems open) the password/group database on every call; a traversal repeats
 * the same handful of ids across thousands of files, so memoise them. The
 * returned name is owned by the cache and valid for the process lifetime;
 * NULL means no such user/group. The mapping is stable for the run, so output
 * is byte-identical to calling getpwuid/getgrgid directly.
 *
 * NOT thread-safe: called only from the (single) eval thread (the worker pool
 * runs stat_worker only; -printf/-ls render after the join), and getpwuid/
 * getgrgid are themselves non-reentrant.
 */
const char *frt_uid_name(uid_t uid);
const char *frt_gid_name(gid_t gid);

#endif /* FRT_IDCACHE_H */
