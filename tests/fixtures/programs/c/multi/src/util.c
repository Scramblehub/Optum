#include "util.h"

/* static: only visible in this file; app.c has its own helper. */
static int helper(int x) { return x < 0 ? -x : x; }

int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

int checksum(const char *s) {
  int h = 0;
  while (*s) h = h * 31 + helper(*s++);
  return clamp(h, 0, 1 << 20);
}
