/* A main for the http_glue sample, so it links as a program. */
#include <stddef.h>

typedef struct sqlite3 sqlite3;
int sync_once(sqlite3 *db);

int main(void) { return sync_once(NULL); }
