#include <stdio.h>
#include <string.h>

#include "config.h"

void fast_copy(char *dst, const char *src, int n) { memcpy(dst, src, (size_t)n); }

void log_line(const char *msg) { puts(msg); }

/* Only built without HAVE_FAST_COPY: not compiled by default. */
#ifndef HAVE_FAST_COPY
void buf_copy(char *dst, const char *src, int n) {
  int i;
  for (i = 0; i < n; i++) dst[i] = src[i];
}
#endif

#ifdef TRACE_ON
void trace(const char *msg) { fprintf(stderr, "trace: %s\n", msg); }
#endif

static int sum1(const char *s) { return s[0]; }
static int sum2(const char *s) { return s[0] + s[1]; }

/* An #if around an `else if` reads, to the parser, as a function named `if`: the call to sum2
   is still checksum's, and compiled by default. */
int checksum(const char *s, int mode) {
  int sum = 0;
  if (mode == 1) {
    sum = sum1(s);
  }
#ifndef NO_SUM2
  else if (mode == 2) {
    sum = sum2(s);
  }
#endif
  return sum;
}
