/* Built as a shared library with soname libsqlite3.so.0 (see curl_stub.c). */
#include "sqlite3.h"

int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg,
                 char **errmsg) {
  return (void)db, (void)sql, (void)callback, (void)arg, (void)errmsg, 0;
}
