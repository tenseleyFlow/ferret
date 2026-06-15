#ifndef FRT_DIAG_H
#define FRT_DIAG_H

/*
 * Diagnostics with gnulib-style locale quoting (audit 01): paths are wrapped in
 * U+2018/U+2019 under a UTF-8 locale, ASCII '...' otherwise. Program name token
 * is "ferret:" (the golden suite normalizes it). Call frt_diag_init() once after
 * setlocale().
 */

void frt_diag_init(void);

/* "ferret: <prefix><quoted path>: <strerror(err)>\n" */
void frt_diag_errno(const char *prefix, const char *path, int err);

/* "ferret: <prefix><quoted path>: <suffix>\n" (suffix may be NULL -> no ": ...") */
void frt_diag_msg(const char *prefix, const char *path, const char *suffix);

/* 1 if the current locale uses UTF-8 (for callers building custom messages). */
int frt_diag_utf8(void);

#endif /* FRT_DIAG_H */
