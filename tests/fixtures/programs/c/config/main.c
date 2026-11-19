#include "config.h"

/* A debug-only function, and a macro of the same name otherwise. */
#ifdef SELF_TEST
static void check(int ok) {
  if (!ok) log_line("check failed");
}
#else
#  define check(ok) ((void)(ok))
#endif

int main(void) {
  char a[4] = "abc", b[4];
  buf_copy(b, a, 4); /* fast_copy */
  TRACE("copied");   /* log_line */
  check(b[0] == 'a');
#ifdef VERBOSE
  log_line("verbose");
#endif
  return checksum(b, 2) == 0;
}
