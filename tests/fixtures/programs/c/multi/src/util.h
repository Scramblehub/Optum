/* A project header: prototypes (not definitions) and one static inline definition. */
#ifndef UTIL_H
#define UTIL_H

int clamp(int x, int lo, int hi);
int checksum(const char *s);

static inline int twice(int x) { return x * 2; }

#endif
