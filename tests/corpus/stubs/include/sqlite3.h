/* Stand-in for <sqlite3.h> (see curl/curl.h here): just what the samples use. */
#ifndef OPTUM_STUB_SQLITE3_H
#define OPTUM_STUB_SQLITE3_H

typedef struct sqlite3 sqlite3;
int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg,
                 char **errmsg);

#endif
